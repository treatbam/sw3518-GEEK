#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>

// Own-network Wi-Fi observability (AP scan + channel activity). No spoofing.
namespace RadioTools {

static constexpr size_t kMaxAps = 8;
static constexpr size_t kChannels = 14;  // 1..13 used; index 0 unused
static constexpr size_t kHeatCols = 48;

struct ApRow {
  char ssid[20];
  int32_t rssi = -127;
  uint8_t channel = 0;
  uint8_t auth = 0;
  bool ok = false;
};

void begin();
void enter();
void leave();
void tick(uint32_t now);
void requestScan();

uint8_t apCount();
const ApRow* apAt(uint8_t i);
bool scanning();
uint32_t lastScanMs();

void drawApList(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t bar, uint16_t bg);
void drawChannelHeat(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t hot, uint16_t bg);
void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);
void drawSys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg,
             bool wifiUp, int8_t wifiRssi);

size_t jsonStatus(char* out, size_t n);

}  // namespace RadioTools
