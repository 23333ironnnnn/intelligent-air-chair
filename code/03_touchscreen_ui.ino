/*
 * 智能气垫椅 - ESP32 触摸屏温控 UI
 * ============================================================
 * 实际硬件: Sunton ESP32-3248S035R  (社区俗称 "黄色屏 / CYD")
 *   - 主控: ESP32-WROOM-32E  (注意: 是老款 ESP32, 不是 S3!)
 *   - 屏幕: 3.5" ST7796, 320x480, SPI
 *   - 触摸: XPT2046 电阻触摸 (需要校准)
 *
 * 功能: 可视化温度显示 + 触摸调档 (替代 02 版的 Serial 命令)
 *   - 低温 30°C / 中温 34°C / 高温 38°C
 *   - 高温档 15 分钟后自动降到中温档 (沿用 02 版逻辑)
 *
 * 依赖库 (Arduino 库管理器安装):
 *   - LovyanGFX
 *   - OneWire
 *   - DallasTemperature
 *
 * 开发板选择: "ESP32 Dev Module"  (不是 S3!)
 * ============================================================
 */

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ========== 外设引脚 (空闲脚, 来自板子 I2C 4pin 接口) ==========
#define PIN_DS18B20   25         // DS18B20 数据线  (I2C 座子的 IO25)
#define PIN_HEATER    32         // 4 路 MOS 栅极并联到这一根 (I2C 座子的 IO32)
// I2C 座子同时提供 3.3V 和 GND, 接线见实施文档

// ========== 板载 RGB 状态灯 (官方资料: 共阳, LOW=亮) ==========
// 用作档位指示: 低温=蓝 / 中温=绿 / 高温=红, 不看屏也知道在哪档
#define PIN_LED_R     22
#define PIN_LED_G     16
#define PIN_LED_B     17

// ========== 屏幕配置 (ST7796 + XPT2046 电阻触摸) ==========
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX(void) {
    { // SPI 总线 (显示)
      auto cfg = _bus.config();
      cfg.spi_host    = HSPI_HOST;     // CYD 显示走 HSPI
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 14;
      cfg.pin_mosi    = 13;
      cfg.pin_miso    = 12;
      cfg.pin_dc      = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // 面板
      auto cfg = _panel.config();
      cfg.pin_cs       = 15;
      cfg.pin_rst      = -1;
      cfg.pin_busy     = -1;
      cfg.panel_width  = 320;
      cfg.panel_height = 480;
      cfg.offset_x     = 0;
      cfg.offset_y     = 0;
      cfg.readable     = true;
      cfg.invert       = true;   // ⚠️ ST7796 多数需 true; 若颜色反相改 false
      cfg.rgb_order    = false;  // ⚠️ 若红蓝互换改 true
      cfg.dlen_16bit   = false;
      cfg.bus_shared   = true;
      _panel.config(cfg);
    }
    { // 背光
      auto cfg = _light.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq   = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    { // XPT2046 电阻触摸 (与显示共用 HSPI 总线)
      auto cfg = _touch.config();
      cfg.x_min = 300;  cfg.x_max = 3900;
      cfg.y_min = 200;  cfg.y_max = 3700;
      cfg.pin_int = 36;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = HSPI_HOST;
      cfg.freq     = 1000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs   = 33;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Preferences prefs;

// ========== 温控状态 (沿用 02 版) ==========
int targetTemp = 30;
unsigned long boostStartTime = 0;
bool boostMode = false;
const unsigned long BOOST_DURATION = 900000UL;   // 15 分钟

float currentTemp = 0;
int currentPwm = 0;
unsigned long lastSample = 0;

// ========== UI 布局 (横屏 480x320) ==========
const int SCR_W = 480, SCR_H = 320;
struct Button { int x, y, w, h; int temp; const char* label; uint16_t color; };
Button buttons[3] = {
  {  24, 210, 130, 80, 30, "30 C", TFT_BLUE  },
  { 175, 210, 130, 80, 34, "34 C", TFT_GREEN },
  { 326, 210, 130, 80, 38, "38 C", TFT_RED   },
};

// ---------- 触摸校准: 存到 NVS, 没有就引导校准 ----------
void setupTouch() {
  uint16_t calData[8];
  prefs.begin("touch", true);
  bool ok = prefs.getBytesLength("cal") == sizeof(calData);
  if (ok) prefs.getBytes("cal", calData, sizeof(calData));
  prefs.end();

  if (ok) {
    tft.setTouchCalibrate(calData);
    return;
  }
  // 首次运行: 引导点击四角校准
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(middle_center);
  tft.setTextSize(2);
  tft.drawString("Touch the arrows to calibrate", SCR_W / 2, SCR_H / 2);
  delay(800);
  tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 30);

  prefs.begin("touch", false);
  prefs.putBytes("cal", calData, sizeof(calData));
  prefs.end();
}

// 板载 RGB 灯指示当前档位 (共阳: LOW 点亮)
void setStatusLED(int t) {
  digitalWrite(PIN_LED_R, (t == 38) ? LOW : HIGH);   // 高温=红
  digitalWrite(PIN_LED_G, (t == 34) ? LOW : HIGH);   // 中温=绿
  digitalWrite(PIN_LED_B, (t == 30) ? LOW : HIGH);   // 低温=蓝
}

void setMode(int t) {
  targetTemp = t;
  boostMode = (t == 38);
  if (boostMode) boostStartTime = millis();
  setStatusLED(t);
  drawButtons();
  drawStatus();
}

void drawButtons() {
  for (int i = 0; i < 3; i++) {
    bool active = (buttons[i].temp == targetTemp);
    tft.fillRoundRect(buttons[i].x, buttons[i].y, buttons[i].w, buttons[i].h, 10,
                      active ? buttons[i].color : TFT_DARKGREY);
    tft.drawRoundRect(buttons[i].x, buttons[i].y, buttons[i].w, buttons[i].h, 10,
                      active ? TFT_WHITE : TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(middle_center);
    tft.setTextSize(3);
    tft.drawString(buttons[i].label,
                   buttons[i].x + buttons[i].w / 2,
                   buttons[i].y + buttons[i].h / 2);
  }
}

void drawStatus() {
  tft.fillRect(0, 160, SCR_W, 36, TFT_BLACK);
  tft.setTextColor(TFT_CYAN);
  tft.setTextDatum(middle_center);
  tft.setTextSize(3);
  char buf[48];
  if (boostMode) {
    long remain = (long)(BOOST_DURATION - (millis() - boostStartTime)) / 1000;
    if (remain < 0) remain = 0;
    snprintf(buf, sizeof(buf), "Boost  %ld:%02ld   PWM %d", remain / 60, remain % 60, currentPwm);
  } else {
    snprintf(buf, sizeof(buf), "Target %dC   PWM %d", targetTemp, currentPwm);
  }
  tft.drawString(buf, SCR_W / 2, 178);
}

void drawTemp() {
  tft.fillRect(0, 40, SCR_W, 110, TFT_BLACK);
  tft.setTextColor(currentTemp < targetTemp - 0.5 ? TFT_ORANGE : TFT_GREENYELLOW);
  tft.setTextDatum(middle_center);
  tft.setTextSize(9);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", currentTemp);
  tft.drawString(buf, SCR_W / 2 - 24, 95);
  tft.setTextSize(4);
  tft.drawString("C", SCR_W / 2 + 150, 70);
}

void setup() {
  Serial.begin(115200);
  sensors.begin();

  pinMode(PIN_HEATER, OUTPUT);
  analogWrite(PIN_HEATER, 0);

  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setStatusLED(targetTemp);    // 初始档位指示

  tft.init();
  tft.setRotation(1);          // 横屏 480x320
  tft.setBrightness(200);

  setupTouch();                // 触摸校准 (首次需点四角)

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(middle_center);
  tft.setTextSize(3);
  tft.drawString("Air Chair Heater", SCR_W / 2, 20);

  drawTemp();
  drawStatus();
  drawButtons();
}

void loop() {
  // ---- 触摸检测 ----
  int32_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    for (int i = 0; i < 3; i++) {
      if (tx >= buttons[i].x && tx <= buttons[i].x + buttons[i].w &&
          ty >= buttons[i].y && ty <= buttons[i].y + buttons[i].h) {
        setMode(buttons[i].temp);
        Serial.printf("-> mode %dC\n", buttons[i].temp);
        delay(250);            // 简单防抖
      }
    }
  }

  // ---- 每秒采样 + 控温 ----
  if (millis() - lastSample >= 1000) {
    lastSample = millis();

    // 高温档 15 分钟自动降档
    if (boostMode && (millis() - boostStartTime > BOOST_DURATION)) {
      targetTemp = 34;
      boostMode = false;
      setStatusLED(targetTemp);
      drawButtons();
      Serial.println("-> Auto-downgrade to Mid (34C)");
    }

    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t > -50 && t < 125) currentTemp = t;   // 过滤无效读数

    // 控温逻辑 (沿用 02 版)
    if (currentTemp < targetTemp - 0.5)      currentPwm = 255;
    else if (currentTemp >= targetTemp)      currentPwm = 0;
    else currentPwm = map(int((targetTemp - currentTemp) * 100), 0, 50, 0, 100);

    analogWrite(PIN_HEATER, currentPwm);       // 一根线驱动 4 路 MOS

    drawTemp();
    drawStatus();
    Serial.printf("Temp:%.1fC Target:%dC PWM:%d\n", currentTemp, targetTemp, currentPwm);
  }
}
