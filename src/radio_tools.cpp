#include "radio_tools.h"
#include <string.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

namespace RadioTools {
namespace {

ApRow aps[kMaxAps];
uint8_t nAps = 0;
BleRow bles[kMaxBle];
uint8_t nBle = 0;

bool wifiScanBusy = false;
bool bleScanBusy = false;
bool bleReady = false;
Focus focus = Focus::Idle;
bool active = false;
uint32_t nextWifiScan = 0;
uint32_t nextBleScan = 0;
static constexpr uint32_t kWifiPeriodMs = 5000;
static constexpr uint32_t kBlePeriodMs = 4000;

uint8_t heat[kHeatCols][kChannels];
size_t heatHead = 0;

BLEScan* bleScan = nullptr;

void text(Adafruit_GFX& g, int x, int y, const char* s, uint16_t c, uint16_t bg, uint8_t size = 1) {
  g.setTextSize(size);
  g.setTextColor(c, bg);
  g.setTextWrap(false);
  g.setCursor(x, y);
  g.print(s);
}

void pushHeatFromScan() {
  uint16_t score[kChannels] = {};
  for (uint8_t i = 0; i < nAps; i++) {
    const uint8_t ch = aps[i].channel;
    if (ch == 0 || ch >= kChannels) continue;
    int s = (int)aps[i].rssi + 90;
    if (s < 0) s = 0;
    if (s > 60) s = 60;
    score[ch] = (uint16_t)(score[ch] + (uint16_t)s + 10);
  }
  uint16_t mx = 1;
  for (uint8_t c = 1; c < kChannels; c++) {
    if (score[c] > mx) mx = score[c];
  }
  for (uint8_t c = 0; c < kChannels; c++) {
    heat[heatHead][c] = (uint8_t)((score[c] * 255) / mx);
  }
  heatHead = (heatHead + 1) % kHeatCols;
}

void ingestWifiScan() {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  wifiScanBusy = false;
  nAps = 0;
  if (n <= 0) {
    WiFi.scanDelete();
    pushHeatFromScan();
    return;
  }
  bool used[64] = {};
  const int lim = n > 64 ? 64 : n;
  for (uint8_t slot = 0; slot < kMaxAps; slot++) {
    int best = -1;
    int32_t bestRssi = -999;
    for (int i = 0; i < lim; i++) {
      if (used[i]) continue;
      const int32_t r = WiFi.RSSI(i);
      if (r > bestRssi) {
        bestRssi = r;
        best = i;
      }
    }
    if (best < 0) break;
    used[best] = true;
    ApRow& a = aps[nAps++];
    a.ok = true;
    a.rssi = bestRssi;
    a.channel = (uint8_t)WiFi.channel(best);
    a.auth = (uint8_t)WiFi.encryptionType(best);
    String ss = WiFi.SSID(best);
    if (ss.isEmpty()) ss = "<hidden>";
    strncpy(a.ssid, ss.c_str(), sizeof(a.ssid) - 1);
    a.ssid[sizeof(a.ssid) - 1] = 0;
  }
  WiFi.scanDelete();
  pushHeatFromScan();
}

void ensureBle() {
  if (bleReady) return;
  BLEDevice::init("");
  bleScan = BLEDevice::getScan();
  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(80);
  bleReady = true;
}

void runBleScanBlocking() {
  if (!bleScan) return;
  bleScanBusy = true;
  // duration seconds; compatible with Arduino-ESP32 2.x Bluedroid BLE
  BLEScanResults found = bleScan->start(2, false);
  nBle = 0;
  const int n = found.getCount();
  bool used[32] = {};
  const int lim = n > 32 ? 32 : n;
  for (uint8_t slot = 0; slot < kMaxBle; slot++) {
    int best = -1;
    int bestRssi = -999;
    for (int i = 0; i < lim; i++) {
      if (used[i]) continue;
      BLEAdvertisedDevice d = found.getDevice(i);
      const int r = d.getRSSI();
      if (r > bestRssi) {
        bestRssi = r;
        best = i;
      }
    }
    if (best < 0) break;
    used[best] = true;
    BLEAdvertisedDevice d = found.getDevice(best);
    BleRow& b = bles[nBle++];
    b.ok = true;
    b.rssi = d.getRSSI();
    String name;
    if (d.haveName()) name = String(d.getName().c_str());
    else name = String(d.getAddress().toString().c_str());
    if (name.length() > 17) name = name.substring(0, 17);
    strncpy(b.name, name.c_str(), sizeof(b.name) - 1);
    b.name[sizeof(b.name) - 1] = 0;
  }
  bleScan->clearResults();
  bleScanBusy = false;
}

int rssiBarW(int32_t rssi, int maxW) {
  int w = (int)rssi + 90;  // -90..-30
  if (w < 0) w = 0;
  if (w > 60) w = 60;
  return w * maxW / 60;
}

}  // namespace

void begin() {
  memset(heat, 0, sizeof(heat));
  heatHead = 0;
  nAps = 0;
  nBle = 0;
}

void enter() {
  active = true;
  nextWifiScan = 0;
  nextBleScan = 0;
}

void leave() {
  active = false;
  focus = Focus::Idle;
  if (wifiScanBusy) {
    WiFi.scanDelete();
    wifiScanBusy = false;
  }
  if (bleScan && bleScanBusy) {
    bleScan->stop();
    bleScanBusy = false;
  }
}

void setFocus(Focus f) { focus = f; }

void requestScan() {
  nextWifiScan = 0;
  nextBleScan = 0;
}

void tick(uint32_t now) {
  if (!active) return;

  if (focus == Focus::Wifi) {
    if (wifiScanBusy) {
      ingestWifiScan();
    } else if (now >= nextWifiScan) {
      const int16_t r = WiFi.scanNetworks(true, true);
      if (r == WIFI_SCAN_RUNNING || r >= 0) {
        wifiScanBusy = true;
        nextWifiScan = now + kWifiPeriodMs;
      } else {
        nextWifiScan = now + 2000;
      }
    }
  } else if (focus == Focus::Ble) {
    ensureBle();
    if (now >= nextBleScan) {
      runBleScanBlocking();
      nextBleScan = now + kBlePeriodMs;
    }
  }
}

uint8_t apCount() { return nAps; }
const ApRow* apAt(uint8_t i) { return (i < nAps) ? &aps[i] : nullptr; }
uint8_t bleCount() { return nBle; }
const BleRow* bleAt(uint8_t i) { return (i < nBle) ? &bles[i] : nullptr; }
bool scanning() { return wifiScanBusy || bleScanBusy; }

void drawApList(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t bar, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 14, "WI-FI APS", 0x07FF, bg, 1);
  char line[36];
  snprintf(line, sizeof(line), wifiScanBusy ? "scan..." : "%u found", (unsigned)nAps);
  text(g, 150, 14, line, dim, bg, 1);

  if (nAps == 0) {
    text(g, 4, 56, "waiting for beacons", dim, bg, 1);
    text(g, 4, 118, "short next  long rescan  x3 exit", dim, bg, 1);
    return;
  }

  for (uint8_t i = 0; i < nAps && i < 5; i++) {
    const ApRow& a = aps[i];
    const int y = 28 + (int)i * 18;
    // rank
    snprintf(line, sizeof(line), "%u", (unsigned)(i + 1));
    text(g, 4, y, line, dim, bg, 1);
    text(g, 16, y, a.ssid, fg, bg, 1);
    snprintf(line, sizeof(line), "ch%u", (unsigned)a.channel);
    text(g, 150, y, line, dim, bg, 1);
    const int bw = rssiBarW(a.rssi, 120);
    g.fillRect(16, y + 10, bw, 5, bar);
    g.drawRect(16, y + 10, 120, 5, dim);
    snprintf(line, sizeof(line), "%d", (int)a.rssi);
    text(g, 150, y + 8, line, fg, bg, 1);
  }
  text(g, 4, 124, "short next  long rescan", dim, bg, 1);
}

void drawWaterfall(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t hot, uint16_t accent,
                   uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 14, "WATERFALL", accent, bg, 1);
  text(g, 130, 14, wifiScanBusy ? "hop..." : "ch 1-13", dim, bg, 1);

  const int x0 = 22, y0 = 28, cw = 3, ch = 7;
  for (size_t col = 0; col < kHeatCols; col++) {
    const size_t src = (heatHead + 1 + col) % kHeatCols;
    for (uint8_t c = 1; c <= 13; c++) {
      const uint8_t v = heat[src][c];
      if (v < 6) continue;
      uint16_t color = dim;
      if (v > 50) color = fg;
      if (v > 110) color = hot;
      if (v > 180) color = accent;
      g.fillRect(x0 + (int)col * cw, y0 + (int)(c - 1) * ch, cw - 1, ch - 1, color);
    }
  }
  for (uint8_t c = 1; c <= 13; c += 2) {
    char lab[4];
    snprintf(lab, sizeof(lab), "%u", (unsigned)c);
    text(g, 4, y0 + (int)(c - 1) * ch, lab, dim, bg, 1);
  }
  // Top APs strip
  if (nAps) {
    char line[40];
    snprintf(line, sizeof(line), "%s %ddB", aps[0].ssid, (int)aps[0].rssi);
    text(g, 4, 122, line, fg, bg, 1);
  } else {
    text(g, 4, 122, "beacon heat (not packet sniff)", dim, bg, 1);
  }
}

void drawBleList(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t bar, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 14, "BLE SCAN", 0x07E0, bg, 1);
  char line[36];
  snprintf(line, sizeof(line), bleScanBusy ? "scan..." : "%u found", (unsigned)nBle);
  text(g, 150, 14, line, dim, bg, 1);

  if (nBle == 0) {
    text(g, 4, 56, "no advertisers yet", dim, bg, 1);
    text(g, 4, 118, "short next  long rescan  x3 exit", dim, bg, 1);
    return;
  }

  for (uint8_t i = 0; i < nBle && i < 5; i++) {
    const BleRow& b = bles[i];
    const int y = 28 + (int)i * 18;
    snprintf(line, sizeof(line), "%u", (unsigned)(i + 1));
    text(g, 4, y, line, dim, bg, 1);
    text(g, 16, y, b.name, fg, bg, 1);
    const int bw = rssiBarW(b.rssi, 120);
    g.fillRect(16, y + 10, bw, 5, bar);
    g.drawRect(16, y + 10, 120, 5, dim);
    snprintf(line, sizeof(line), "%d", (int)b.rssi);
    text(g, 150, y + 8, line, fg, bg, 1);
  }
  text(g, 4, 124, "advert RSSI — not connections", dim, bg, 1);
}

void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 14, "RADIO HELP", accent, bg, 1);
  text(g, 4, 28, "pages: Wi-Fi / Fall / BLE / Sys", fg, bg, 1);
  text(g, 4, 42, "short : next   double : prev", fg, bg, 1);
  text(g, 4, 54, "long  : rescan", fg, bg, 1);
  text(g, 4, 66, "triple: back to charger", fg, bg, 1);
  text(g, 4, 84, "Wi-Fi = beacon APs + heat", dim, bg, 1);
  text(g, 4, 96, "BLE = nearby advertisers", dim, bg, 1);
  text(g, 4, 118, "web: /radio", accent, bg, 1);
}

void drawSys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg, bool wifiUp,
             int8_t wifiRssi) {
  g.fillScreen(bg);
  text(g, 4, 14, "SYSTEM", accent, bg, 1);
  char line[48];
  const uint32_t sec = millis() / 1000;
  snprintf(line, sizeof(line), "up %luh %lum", (unsigned long)(sec / 3600),
           (unsigned long)((sec / 60) % 60));
  text(g, 4, 30, line, fg, bg, 1);
  snprintf(line, sizeof(line), "heap %u", (unsigned)ESP.getFreeHeap());
  text(g, 4, 44, line, fg, bg, 1);
  if (wifiUp) {
    snprintf(line, sizeof(line), "wifi %s", WiFi.localIP().toString().c_str());
    text(g, 4, 58, line, fg, bg, 1);
    snprintf(line, sizeof(line), "rssi %d dBm", (int)wifiRssi);
    text(g, 4, 72, line, fg, bg, 1);
  } else {
    text(g, 4, 58, "wifi down / scanning", dim, bg, 1);
  }
  snprintf(line, sizeof(line), "AP %u  BLE %u", (unsigned)nAps, (unsigned)nBle);
  text(g, 4, 90, line, fg, bg, 1);
  text(g, 4, 118, "x3 charger", dim, bg, 1);
}

size_t jsonStatus(char* out, size_t n) {
  size_t o = 0;
  auto append = [&](const char* s) {
    while (*s && o + 1 < n) out[o++] = *s++;
  };
  append("{\"wifi_scan\":");
  append(wifiScanBusy ? "true" : "false");
  append(",\"ble_scan\":");
  append(bleScanBusy ? "true" : "false");
  append(",\"aps\":[");
  for (uint8_t i = 0; i < nAps; i++) {
    if (i) append(",");
    char safe[20];
    size_t j = 0;
    for (const char* sp = aps[i].ssid; *sp && j + 1 < sizeof(safe); ++sp) {
      if (*sp == '"' || *sp == '\\') continue;
      safe[j++] = *sp;
    }
    safe[j] = 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u}", safe, (int)aps[i].rssi,
             (unsigned)aps[i].channel);
    append(buf);
  }
  append("],\"ble\":[");
  for (uint8_t i = 0; i < nBle; i++) {
    if (i) append(",");
    char safe[20];
    size_t j = 0;
    for (const char* sp = bles[i].name; *sp && j + 1 < sizeof(safe); ++sp) {
      if (*sp == '"' || *sp == '\\') continue;
      safe[j++] = *sp;
    }
    safe[j] = 0;
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"name\":\"%s\",\"rssi\":%d}", safe, (int)bles[i].rssi);
    append(buf);
  }
  append("]}");
  out[o] = 0;
  return o;
}

}  // namespace RadioTools
