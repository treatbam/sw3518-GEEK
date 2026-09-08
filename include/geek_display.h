#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "pins.h"

static constexpr uint16_t COL_BLACK     = 0x0000;
static constexpr uint16_t COL_WHITE     = 0xFFFF;
static constexpr uint16_t COL_RED       = 0xF800;
static constexpr uint16_t COL_GREEN     = 0x07E0;
static constexpr uint16_t COL_CYAN      = 0x07FF;
static constexpr uint16_t COL_YELLOW    = 0xFFE0;
static constexpr uint16_t COL_MAGENTA   = 0xF81F;
static constexpr uint16_t COL_ORANGE    = 0xFD20;
static constexpr uint16_t COL_DARKGREY  = 0x7BEF;
static constexpr uint16_t COL_LIGHTGREY = 0xC618;
static constexpr uint16_t COL_DIM       = 0x4208;  // very dim grey

inline void gfxText(Adafruit_GFX& g, int16_t x, int16_t y, const char* s,
                    uint16_t fg, uint16_t bg, uint8_t size,
                    bool centerX = false, bool right = false) {
  g.setTextSize(size);
  g.setTextColor(fg, bg);
  g.setTextWrap(false);
  const int16_t tw = (int16_t)(strlen(s) * 6 * size);
  if (centerX) x = x - tw / 2;
  else if (right) x = x - tw;
  g.setCursor(x, y);
  g.print(s);
}

// Wi-Fi arcs + MQTT diamond + WWW glyph. Colors: bright vs dim.
inline void drawWifiIcon(Adafruit_GFX& g, int16_t x, int16_t y, int bars, uint16_t on, uint16_t off) {
  // bars 0..4 (0 = disconnected)
  for (int i = 0; i < 4; i++) {
    uint16_t c = (bars > i) ? on : off;
    int h = 2 + i * 2;
    g.fillRect(x + i * 3, y + 8 - h, 2, h, c);
  }
}

inline void drawMqttIcon(Adafruit_GFX& g, int16_t x, int16_t y, bool ok, uint16_t on, uint16_t off) {
  uint16_t c = ok ? on : off;
  g.fillCircle(x + 4, y + 5, 2, c);
  g.drawCircle(x + 4, y + 5, 4, c);
}

inline void drawWebIcon(Adafruit_GFX& g, int16_t x, int16_t y, bool active, uint16_t on, uint16_t off) {
  uint16_t c = active ? on : off;
  g.drawCircle(x + 5, y + 5, 4, c);
  g.drawLine(x + 1, y + 5, x + 9, y + 5, c);
  g.drawLine(x + 5, y + 1, x + 5, y + 9, c);
  // tiny "W" cue
  g.drawPixel(x + 3, y + 7, c);
  g.drawPixel(x + 5, y + 6, c);
  g.drawPixel(x + 7, y + 7, c);
}

class GeekDisplay : public Adafruit_ST7789 {
 public:
  GeekDisplay() : Adafruit_ST7789(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST) {}

  void beginPanel() {
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    SPI.setFrequency(27000000);
    SPI.setDataMode(SPI_MODE3);
    SPI.setBitOrder(MSBFIRST);
    init(135, 240);
    setSPISpeed(27000000);
    setRotation(1);
    invertDisplay(true);
  }

  int16_t W() { return width(); }
  int16_t H() { return height(); }

  void push(GFXcanvas16& frame) {
    drawRGBBitmap(0, 0, frame.getBuffer(), frame.width(), frame.height());
  }
};
