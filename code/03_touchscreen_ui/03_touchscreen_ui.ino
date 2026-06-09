/*
 * 智能气垫椅 / 飞机座椅 - ESP32 触摸屏温控 UI (高级白色航空版)
 * ============================================================
 * 硬件: Sunton ESP32-3248S035R (3.5" ST7796U 320x480 + XPT2046 电阻触摸)
 * 风格: 纯白底 + 深海军蓝字 + 圆角卡片 + 平滑矢量字体
 *
 * 功能: 座椅温度可视化 + 触摸调档 (30/34/38C)
 *   - 高温档 15 分钟自动降到中温档
 *   - 板载 RGB 灯指示档位
 * 开发板: "ESP32 Dev Module" | 上传速度建议 115200
 * 依赖库: LovyanGFX / OneWire / DallasTemperature
 * ============================================================
 */

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ========== 外设引脚 (板子 I2C 4pin 座子) ==========
#define PIN_DS18B20   25
#define PIN_HEATER    32

// 板载 RGB 状态灯 (共阳: LOW=亮)
#define PIN_LED_R     22
#define PIN_LED_G     16
#define PIN_LED_B     17

// ========== 屏幕配置 (ST7796 + XPT2046) ==========
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_XPT2046 _touch;
public:
  LGFX(void) {
    { auto cfg = _bus.config();
      cfg.spi_host = HSPI_HOST; cfg.spi_mode = 0;
      cfg.freq_write = 40000000; cfg.freq_read = 16000000;
      cfg.spi_3wire = false; cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14; cfg.pin_mosi = 13; cfg.pin_miso = 12; cfg.pin_dc = 2;
      _bus.config(cfg); _panel.setBus(&_bus); }
    { auto cfg = _panel.config();
      cfg.pin_cs = 15; cfg.pin_rst = -1; cfg.pin_busy = -1;
      cfg.panel_width = 320; cfg.panel_height = 480;
      cfg.readable = true; cfg.invert = false; cfg.rgb_order = false;
      cfg.dlen_16bit = false; cfg.bus_shared = true;
      _panel.config(cfg); }
    { auto cfg = _light.config();
      cfg.pin_bl = 27; cfg.invert = false; cfg.freq = 44100; cfg.pwm_channel = 7;
      _light.config(cfg); _panel.setLight(&_light); }
    { auto cfg = _touch.config();
      cfg.x_min = 300; cfg.x_max = 3900; cfg.y_min = 200; cfg.y_max = 3700;
      cfg.pin_int = 36; cfg.bus_shared = true; cfg.offset_rotation = 0;
      cfg.spi_host = HSPI_HOST; cfg.freq = 1000000;
      cfg.pin_sclk = 14; cfg.pin_mosi = 13; cfg.pin_miso = 12; cfg.pin_cs = 33;
      _touch.config(cfg); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};

LGFX tft;
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Preferences prefs;

// ========== 配色 (纯白 + 深蓝, 航空高级感) ==========
uint16_t C_BG, C_NAVY, C_ACCENT, C_LINE, C_SUB, C_HEAT, C_WHITE;

// ========== 温控状态 ==========
int targetTemp = 30;
unsigned long boostStartTime = 0;
bool boostMode = false;
const unsigned long BOOST_DURATION = 900000UL;   // 15 分钟
float currentTemp = 0;
int currentPwm = 0;
unsigned long lastSample = 0;

// ========== UI 布局 (横屏 480x320) ==========
const int SCR_W = 480, SCR_H = 320;

// ---- 拖动滑块: 3 个吸附档位 ----
const int   TRK_LEFT = 70, TRK_RIGHT = 410;   // 轨道两端
const int   TRK_CY   = 280, TRK_H = 14;        // 轨道中心 y / 粗细
const int   snapX[3]  = {110, 240, 370};       // 三档吸附点 x
const int   temps[3]  = {30, 34, 38};
const char* sublabels[3] = {"LOW", "COMFORT", "WARM"};
int sliderX = 110;                             // 当前手柄 x

int nearestSnap(int x) {
  int best = 0, bd = 999999;
  for (int i = 0; i < 3; i++) { int d = abs(x - snapX[i]); if (d < bd) { bd = d; best = i; } }
  return best;
}
int snapXForTemp(int t) {
  for (int i = 0; i < 3; i++) if (temps[i] == t) return snapX[i];
  return snapX[0];
}

// 前置声明
void drawSlider(bool dragging);
void drawStatus();
void drawTemp();

// 画一个度数小圆环 (避免字体缺 ° 字形)
void drawDegree(int x, int y, int r, uint16_t color) {
  tft.drawCircle(x, y, r, color);
  tft.drawCircle(x, y, r - 1, color);
}

// ---------- 触摸校准 ----------
void setupTouch() {
  uint16_t calData[8];
  prefs.begin("touch", true);
  bool ok = prefs.getBytesLength("cal") == sizeof(calData);
  if (ok) prefs.getBytes("cal", calData, sizeof(calData));
  prefs.end();
  if (ok) { tft.setTouchCalibrate(calData); return; }

  tft.fillScreen(C_BG);
  tft.setTextColor(C_NAVY, C_BG);
  tft.setFont(&fonts::FreeSans12pt7b);
  tft.setTextDatum(middle_center);
  tft.drawString("Tap the corners to calibrate", SCR_W / 2, SCR_H / 2);
  delay(900);
  tft.calibrateTouch(calData, C_ACCENT, C_BG, 30);
  prefs.begin("touch", false);
  prefs.putBytes("cal", calData, sizeof(calData));
  prefs.end();
}

// 板载 RGB 灯指示档位 (共阳: LOW 点亮)
void setStatusLED(int t) {
  digitalWrite(PIN_LED_R, (t == 38) ? LOW : HIGH);
  digitalWrite(PIN_LED_G, (t == 34) ? LOW : HIGH);
  digitalWrite(PIN_LED_B, (t == 30) ? LOW : HIGH);
}

void setMode(int t) {
  targetTemp = t;
  boostMode = (t == 38);
  if (boostMode) boostStartTime = millis();
  setStatusLED(t);
  sliderX = snapXForTemp(t);
  drawSlider(false);
  drawStatus();
}

void drawSlider(bool dragging) {
  // 清滑块区域 (状态行 178~212 以下)
  tft.fillRect(0, 214, SCR_W, SCR_H - 214, C_BG);

  int dispTemp = dragging ? temps[nearestSnap(sliderX)] : targetTemp;
  int hx = constrain(sliderX, snapX[0], snapX[2]);
  int top = TRK_CY - TRK_H / 2;

  // 轨道底色 + 已选填充
  tft.fillRoundRect(TRK_LEFT, top, TRK_RIGHT - TRK_LEFT, TRK_H, TRK_H / 2, C_LINE);
  if (hx - TRK_LEFT > TRK_H)
    tft.fillRoundRect(TRK_LEFT, top, hx - TRK_LEFT, TRK_H, TRK_H / 2, C_ACCENT);

  // 三档英文标签
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(middle_center);
  for (int i = 0; i < 3; i++) {
    tft.setTextColor(temps[i] == dispTemp ? C_ACCENT : C_SUB, C_BG);
    tft.drawString(sublabels[i], snapX[i], TRK_CY + 26);
  }

  // 圆形手柄 (三层同心圆)
  tft.fillCircle(hx, TRK_CY, 18, C_ACCENT);
  tft.fillCircle(hx, TRK_CY, 11, C_WHITE);
  tft.fillCircle(hx, TRK_CY, 7,  C_ACCENT);

  // 手柄上方深蓝气泡 + 数字
  int bw = 54, bh = 32, bx = hx - bw / 2, by = 220;
  if (bx < 2) bx = 2;
  if (bx + bw > SCR_W - 2) bx = SCR_W - 2 - bw;
  tft.fillRoundRect(bx, by, bw, bh, 8, C_NAVY);
  tft.fillTriangle(hx - 6, by + bh - 1, hx + 6, by + bh - 1, hx, by + bh + 8, C_NAVY);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", dispTemp);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(middle_center);
  tft.setTextColor(C_WHITE, C_NAVY);
  tft.drawString(buf, bx + bw / 2, by + bh / 2);
}

void drawStatus() {
  tft.fillRect(0, 178, SCR_W, 34, C_BG);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::FreeSans12pt7b);
  char buf[40];
  if (boostMode) {
    long remain = (long)(BOOST_DURATION - (millis() - boostStartTime)) / 1000;
    if (remain < 0) remain = 0;
    tft.setTextColor(C_ACCENT, C_BG);
    snprintf(buf, sizeof(buf), "BOOST   %ld:%02ld", remain / 60, remain % 60);
    tft.drawString(buf, SCR_W / 2, 195);
  } else if (currentPwm > 0) {
    tft.fillCircle(SCR_W / 2 - 58, 195, 5, C_HEAT);   // 加热指示点
    tft.setTextColor(C_HEAT, C_BG);
    tft.drawString("HEATING", SCR_W / 2 + 6, 195);
  } else {
    tft.setTextColor(C_SUB, C_BG);
    tft.drawString("READY", SCR_W / 2, 195);
  }
}

void drawTemp() {
  tft.fillRect(0, 62, SCR_W, 112, C_BG);
  // 大温度数字
  char buf[12];
  snprintf(buf, sizeof(buf), "%.1f", currentTemp);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextSize(2);
  tft.setTextColor(C_NAVY, C_BG);
  tft.setTextDatum(middle_right);
  int baseX = SCR_W / 2 + 18;
  tft.drawString(buf, baseX, 112);
  tft.setTextSize(1);
  // 度数环 + C
  drawDegree(baseX + 26, 86, 7, C_NAVY);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextDatum(top_left);
  tft.setTextColor(C_NAVY, C_BG);
  tft.drawString("C", baseX + 40, 92);
  // 副标题
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_SUB, C_BG);
  tft.setTextDatum(middle_center);
  tft.drawString("CURRENT TEMPERATURE", SCR_W / 2, 158);
}

void drawFrame() {
  tft.fillScreen(C_BG);
  // 标题
  tft.setFont(&fonts::FreeSans12pt7b);
  tft.setTextColor(C_NAVY, C_BG);
  tft.setTextDatum(middle_center);
  tft.drawString("SEAT TEMPERATURE", SCR_W / 2, 28);
  // 细分隔线
  tft.drawFastHLine(60, 50, SCR_W - 120, C_LINE);
}

void setup() {
  Serial.begin(115200);
  sensors.begin();

  pinMode(PIN_HEATER, OUTPUT);
  analogWrite(PIN_HEATER, 0);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  setStatusLED(targetTemp);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(220);

  // 配色初始化
  C_WHITE  = tft.color565(255, 255, 255);
  C_BG     = tft.color565(255, 255, 255);   // 纯白底
  C_NAVY   = tft.color565(16, 38, 74);      // 深海军蓝 (主文字)
  C_ACCENT = tft.color565(0, 102, 178);     // 航空蓝 (选中)
  C_LINE   = tft.color565(206, 214, 226);   // 浅灰描边/分隔
  C_SUB    = tft.color565(120, 134, 158);   // 次要灰蓝文字
  C_HEAT   = tft.color565(228, 116, 0);     // 暖橙 (加热指示)

  setupTouch();

  sliderX = snapXForTemp(targetTemp);
  drawFrame();
  drawTemp();
  drawStatus();
  drawSlider(false);
}

void loop() {
  static bool dragging = false;
  int32_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    if (ty >= 214) {                 // 触到滑块区域
      dragging = true;
      int hx = constrain((int)tx, snapX[0], snapX[2]);
      if (abs(hx - sliderX) >= 3) { sliderX = hx; drawSlider(true); }  // 跟手实时
    }
  } else if (dragging) {             // 松手 -> 吸附到最近档位并选中
    dragging = false;
    int idx = nearestSnap(sliderX);
    sliderX = snapX[idx];
    setMode(temps[idx]);
    Serial.printf("-> mode %dC\n", temps[idx]);
  }

  if (millis() - lastSample >= 1000) {
    lastSample = millis();

    if (boostMode && (millis() - boostStartTime > BOOST_DURATION)) {
      targetTemp = 34;
      boostMode = false;
      setStatusLED(targetTemp);
      sliderX = snapXForTemp(34);
      drawSlider(false);
      Serial.println("-> Auto-downgrade to Mid (34C)");
    }

    sensors.requestTemperatures();
    float t = sensors.getTempCByIndex(0);
    if (t > -50 && t < 125) currentTemp = t;

    if (currentTemp < targetTemp - 0.5)      currentPwm = 255;
    else if (currentTemp >= targetTemp)      currentPwm = 0;
    else currentPwm = map(int((targetTemp - currentTemp) * 100), 0, 50, 0, 100);

    analogWrite(PIN_HEATER, currentPwm);

    drawTemp();
    drawStatus();
    Serial.printf("Temp:%.1fC Target:%dC PWM:%d\n", currentTemp, targetTemp, currentPwm);
  }
}
