#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "pins.h"

// RGB565 aliases used by the UI
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

class GeekDisplay : public Adafruit_ST7789 {
public:
  GeekDisplay()
      : Adafruit_ST7789(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST) {}

  void beginPanel() {
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);

    // Waveshare DEV_Config: FSPI pins + SPI_MODE3
    SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    SPI.setFrequency(27000000);
    SPI.setDataMode(SPI_MODE3);
    SPI.setBitOrder(MSBFIRST);

    // Adafruit init for 1.14" 135x240 (sets CGRAM offsets)
    init(135, 240);
    setSPISpeed(27000000);
    setRotation(1);  // landscape 240x135 — Waveshare 08_SD_LCD
    invertDisplay(true);  // Waveshare LCD_Init sends 0x21
  }

  int16_t W() { return width(); }
  int16_t H() { return height(); }

  void text(int16_t x, int16_t y, const char* s, uint16_t fg, uint16_t bg,
            uint8_t size, bool centerX = false, bool right = false) {
    setTextSize(size);
    setTextColor(fg, bg);
    setTextWrap(false);
    int16_t tw = (int16_t)(strlen(s) * 6 * size);
    if (centerX) x = x - tw / 2;
    else if (right) x = x - tw;
    setCursor(x, y);
    print(s);
  }
};
