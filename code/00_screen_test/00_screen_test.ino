/*
 * 第 0 步: 屏幕点亮自检
 * ============================================================
 * 目的: 在接任何加热/传感器之前, 先确认屏幕和触摸配置正确。
 * 只插 USB-C, 烧录这个, 屏幕应显示彩色文字; 触摸屏幕串口打印坐标。
 *
 * 硬件: Sunton ESP32-3248S035R (3.5" ST7796 + XPT2046 电阻触摸)
 * 开发板: "ESP32 Dev Module"
 * 依赖库: LovyanGFX
 * ============================================================
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

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

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);          // 横屏 480x320
  tft.setBrightness(200);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setTextDatum(middle_center);
  tft.setTextSize(3);
  tft.drawString("SCREEN OK", 240, 80);

  // 三色条, 用来确认颜色顺序对不对 (从左到右应是 红/绿/蓝)
  tft.fillRect(40,  150, 120, 100, TFT_RED);
  tft.fillRect(180, 150, 120, 100, TFT_GREEN);
  tft.fillRect(320, 150, 120, 100, TFT_BLUE);

  Serial.println("Screen test running. Touch the screen...");
}

void loop() {
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    Serial.printf("touch x=%d y=%d\n", x, y);
    tft.fillCircle(x, y, 4, TFT_YELLOW);
  }
}
