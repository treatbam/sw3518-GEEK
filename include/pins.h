#pragma once

// Waveshare ESP32-S3-GEEK pin map
static const int PIN_TFT_MOSI = 11;
static const int PIN_TFT_SCLK = 12;
static const int PIN_TFT_CS   = 10;
static const int PIN_TFT_DC   = 8;
static const int PIN_TFT_RST  = 9;
static const int PIN_TFT_BL   = 7;  // active-high via SS8050

// I2C header H1 (schematic) — not Waveshare BME demo 7/8
static const int PIN_I2C_SDA = 16;
static const int PIN_I2C_SCL = 17;

static const int PIN_BOOT_BTN = 0;

// Future side buttons + haptic (free GPIOs on S3-GEEK; wire when case is ready)
static const int PIN_BTN_LEFT  = 1;   // INPUT_PULLUP momentary
static const int PIN_BTN_RIGHT = 2;   // INPUT_PULLUP momentary
static const int PIN_HAPTIC    = 13;  // MOSFET/driver to vibration motor

// TF / microSD (SPI3 / HSPI)
static const int PIN_SD_CS   = 34;
static const int PIN_SD_MOSI = 35;
static const int PIN_SD_SCK  = 36;
static const int PIN_SD_MISO = 37;

// Waveshare 3-pin UART header (USB-UART adapter fallback)
static const int PIN_UART_TX = 43;
static const int PIN_UART_RX = 44;

