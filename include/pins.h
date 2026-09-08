#pragma once

// Waveshare ESP32-S3-GEEK pin map
// Display (ST7789, SPI) — Espressif esp-claw / community ESPHome
static const int PIN_TFT_MOSI = 11;
static const int PIN_TFT_SCLK = 12;
static const int PIN_TFT_CS   = 10;
static const int PIN_TFT_DC   = 8;
static const int PIN_TFT_RST  = 9;
static const int PIN_TFT_BL   = 7;  // active-high via SS8050

// Onboard I2C header (schematic H1: GPIO16/17)
// Note: Waveshare BME demo lists SDA=7/SCL=8; that conflicts with LCD DC=8.
// Prefer schematic 16/17 for the 4-pin I2C port.
static const int PIN_I2C_SDA = 16;
static const int PIN_I2C_SCL = 17;

static const int PIN_BOOT_BTN = 0;
