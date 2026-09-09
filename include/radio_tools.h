#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>

// Own-network RF observability (Wi-Fi beacon scan + BLE adverts). No spoofing.
namespace RadioTools {

static constexpr size_t kMaxAps = 8;
static constexpr size_t kMaxBle = 8;
static constexpr size_t kChannels = 14;  // 1..13 used
static constexpr size_t kHeatCols = 56;

enum class Focus : uint8_t { Idle = 0, Wifi = 1, Ble = 2 };

struct ApRow {
  char ssid[18];
  int32_t rssi = -127;
  uint8_t channel = 0;
  uint8_t auth = 0;
  bool ok = false;
};

struct BleRow {
  char name[18];
  int32_t rssi = -127;
  bool ok = false;
};

void begin();
void enter();
void leave();
void setFocus(Focus f);
void tick(uint32_t now);
void requestScan();

uint8_t apCount();
const ApRow* apAt(uint8_t i);
uint8_t bleCount();
const BleRow* bleAt(uint8_t i);
bool scanning();

void drawApList(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t bar, uint16_t bg);
void drawWaterfall(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t hot, uint16_t accent,
                   uint16_t bg);
void drawBleList(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t bar, uint16_t bg);
void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);
void drawSys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg,
             bool wifiUp, int8_t wifiRssi);

size_t jsonStatus(char* out, size_t n);

}  // namespace RadioTools
