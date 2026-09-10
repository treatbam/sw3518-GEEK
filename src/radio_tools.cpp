#include "radio_tools.h"
#include "features.h"
#include <string.h>

#if !HAS_RADIO

namespace RadioTools {
void begin() {}
void enter() {}
void leave() {}
void invalidateBle() {}
void setFocus(Focus) {}
void tick(uint32_t) {}
void requestScan() {}
uint8_t apCount() { return 0; }
const ApRow* apAt(uint8_t) { return nullptr; }
uint8_t bleCount() { return 0; }
const BleRow* bleAt(uint8_t) { return nullptr; }
bool scanning() { return false; }
void drawApList(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawWaterfall(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawBleList(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawHelp(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawSys(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t, bool, int8_t, bool, bool,
             uint32_t, uint16_t) {}
size_t jsonStatus(char* out, size_t n) {
  if (n) out[0] = 0;
  if (n > 2) {
    out[0] = '{';
    out[1] = '}';
    out[2] = 0;
    return 2;
  }
  return 0;
}
}  // namespace RadioTools

#else

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
static constexpr uint32_t kWaterfallDwellMs = 600;

uint8_t heat[kHeatCols][kChannels];
uint8_t heatKind[kHeatCols][kChannels];  // 0 empty, 1 encrypted, 2 open
size_t heatHead = 0;

uint8_t rollCh = 1;
uint8_t dwellCh = 1;
char hotSsid[18] = "";
int32_t hotRssi = -127;
uint8_t hotCh = 0;

BLEScan* bleScan = nullptr;

void text(Adafruit_GFX& g, int x, int y, const char* s, uint16_t c, uint16_t bg, uint8_t size = 1) {
  g.setTextSize(size);
  g.setTextColor(c, bg);
  g.setTextWrap(false);
  g.setCursor(x, y);
  g.print(s);
}

void drawHBar(Adafruit_GFX& g, int x, int y, int maxW, int h, float frac, uint16_t fill,
              uint16_t dim) {
  if (frac < 0.f) frac = 0.f;
  if (frac > 1.f) frac = 1.f;
  const int bw = (int)(frac * maxW + 0.5f);
  g.drawRect(x, y, maxW, h, dim);
  if (bw > 0) g.fillRect(x + 1, y + 1, bw > maxW - 2 ? maxW - 2 : bw, h - 2, fill);
}

bool isOpenAuth(uint8_t auth) {
  // WIFI_AUTH_OPEN == 0 on Arduino-ESP32
  return auth == 0;
}

void pushHeatColumn(const uint16_t score[kChannels], const uint8_t kind[kChannels]) {
  uint16_t mx = 1;
  for (uint8_t c = 1; c < kChannels; c++) {
    if (score[c] > mx) mx = score[c];
  }
  for (uint8_t c = 0; c < kChannels; c++) {
    heat[heatHead][c] = (uint8_t)((score[c] * 255) / mx);
    heatKind[heatHead][c] = kind[c];
  }
  heatHead = (heatHead + 1) % kHeatCols;
}

void pushHeatFromAps() {
  uint16_t score[kChannels] = {};
  uint8_t kind[kChannels] = {};
  for (uint8_t i = 0; i < nAps; i++) {
    const uint8_t ch = aps[i].channel;
    if (ch == 0 || ch >= kChannels) continue;
    int s = (int)aps[i].rssi + 90;
    if (s < 0) s = 0;
    if (s > 60) s = 60;
    score[ch] = (uint16_t)(score[ch] + (uint16_t)s + 10);
    const uint8_t k = isOpenAuth(aps[i].auth) ? 2 : 1;
    if (k > kind[ch]) kind[ch] = k;
  }
  pushHeatColumn(score, kind);
}

// Single-channel dwell: one heat column dominated by rollCh.
void pushHeatFromChannelScan(uint8_t ch) {
  uint16_t score[kChannels] = {};
  uint8_t kind[kChannels] = {};
  hotSsid[0] = 0;
  hotRssi = -127;
  hotCh = ch;
  for (uint8_t i = 0; i < nAps; i++) {
    if (aps[i].channel != ch) continue;
    int s = (int)aps[i].rssi + 90;
    if (s < 0) s = 0;
    if (s > 60) s = 60;
    score[ch] = (uint16_t)(score[ch] + (uint16_t)s + 10);
    const uint8_t k = isOpenAuth(aps[i].auth) ? 2 : 1;
    if (k > kind[ch]) kind[ch] = k;
    if (aps[i].rssi > hotRssi) {
      hotRssi = aps[i].rssi;
      strncpy(hotSsid, aps[i].ssid, sizeof(hotSsid) - 1);
      hotSsid[sizeof(hotSsid) - 1] = 0;
    }
  }
  // Soft neighbor bleed so empty columns still show channel activity context
  if (score[ch] > 0) {
    if (ch > 1) score[ch - 1] = score[ch] / 5;
    if (ch + 1 < kChannels) score[ch + 1] = score[ch] / 5;
  }
  pushHeatColumn(score, kind);
}

void ingestWifiScan(bool channelMode) {
  const int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  wifiScanBusy = false;
  nAps = 0;
  if (n <= 0) {
    WiFi.scanDelete();
    if (channelMode) {
      pushHeatFromChannelScan(dwellCh);
      rollCh = (dwellCh >= 13) ? 1 : (uint8_t)(dwellCh + 1);
    } else {
      pushHeatFromAps();
    }
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
  if (channelMode) {
    pushHeatFromChannelScan(dwellCh);
    rollCh = (dwellCh >= 13) ? 1 : (uint8_t)(dwellCh + 1);
  } else {
    pushHeatFromAps();
  }
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

void ingestBleResults(BLEScanResults found) {
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
  if (bleScan) bleScan->clearResults();
  bleScanBusy = false;
}

void onBleScanDone(BLEScanResults found) { ingestBleResults(found); }

void startBleScan() {
  if (!bleScan || bleScanBusy) return;
  bleScanBusy = true;
  // Async: duration in seconds. Must not block the charger UI thread.
  if (!bleScan->start(2, onBleScanDone, false)) {
    bleScanBusy = false;
  }
}

int rssiBarW(int32_t rssi, int maxW) {
  int w = (int)rssi + 90;  // -90..-30
  if (w < 0) w = 0;
  if (w > 60) w = 60;
  return w * maxW / 60;
}

// Cool dim -> warm -> hot magenta/cyan; open networks lean cyan, enc lean magenta.
uint16_t heatColor(uint8_t v, uint8_t kind, uint16_t fg, uint16_t dim, uint16_t hot,
                   uint16_t accent) {
  if (v < 8) return dim;
  const bool openAp = (kind == 2);
  if (v < 40) return dim;
  if (v < 90) return fg;
  if (v < 160) return openAp ? accent : hot;
  return openAp ? accent : hot;
}

void truncCopy(char* dst, size_t n, const char* src, size_t maxChars) {
  if (n == 0) return;
  size_t i = 0;
  while (src[i] && i + 1 < n && i < maxChars) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

}  // namespace

void begin() {
  memset(heat, 0, sizeof(heat));
  memset(heatKind, 0, sizeof(heatKind));
  heatHead = 0;
  nAps = 0;
  nBle = 0;
  rollCh = 1;
  dwellCh = 1;
  hotSsid[0] = 0;
  hotRssi = -127;
  hotCh = 0;
}

void enter() {
  active = true;
  nextWifiScan = 0;
  nextBleScan = 0;
  rollCh = 1;
  dwellCh = 1;
}

void invalidateBle() {
  bleScan = nullptr;
  bleReady = false;
  bleScanBusy = false;
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
  if (bleReady) {
    BLEDevice::deinit(false);
    bleReady = false;
    bleScan = nullptr;
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
      ingestWifiScan(false);
    } else if (now >= nextWifiScan) {
      const int16_t r = WiFi.scanNetworks(true, true);
      if (r == WIFI_SCAN_RUNNING || r >= 0) {
        wifiScanBusy = true;
        nextWifiScan = now + kWifiPeriodMs;
      } else {
        nextWifiScan = now + 2000;
      }
    }
  } else if (focus == Focus::Waterfall) {
    if (wifiScanBusy) {
      ingestWifiScan(true);
    } else if (now >= nextWifiScan) {
      // Per-channel beacon scan (no promiscuous sniff). channel=N when API allows.
      dwellCh = rollCh;
      const int16_t r = WiFi.scanNetworks(true, true, false, 300, dwellCh);
      if (r == WIFI_SCAN_RUNNING || r >= 0) {
        wifiScanBusy = true;
        nextWifiScan = now + kWaterfallDwellMs;
      } else {
        // Fallback: full scan if channel arg rejected
        const int16_t r2 = WiFi.scanNetworks(true, true);
        if (r2 == WIFI_SCAN_RUNNING || r2 >= 0) {
          wifiScanBusy = true;
          nextWifiScan = now + kWaterfallDwellMs;
        } else {
          nextWifiScan = now + 1000;
        }
      }
    }
  } else if (focus == Focus::Ble) {
    ensureBle();
    if (bleScanBusy) return;
    if (now >= nextBleScan) {
      startBleScan();
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
  char line[48];
  text(g, 4, 14, "WATERFALL", accent, bg, 1);
  snprintf(line, sizeof(line), "dwell CH%u", (unsigned)dwellCh);
  text(g, 100, 14, line, wifiScanBusy ? accent : fg, bg, 1);
  text(g, 178, 14, wifiScanBusy ? "hop" : "1-13", dim, bg, 1);

  // Large heat map under status bar: time columns x channel rows
  const int x0 = 16, y0 = 26, cw = 4, chH = 7;
  // Highlight current dwell channel row
  g.fillRect(0, y0 + (int)(dwellCh - 1) * chH, 15, chH - 1, accent);

  for (size_t col = 0; col < kHeatCols; col++) {
    const size_t src = (heatHead + 1 + col) % kHeatCols;
    for (uint8_t c = 1; c <= 13; c++) {
      const uint8_t v = heat[src][c];
      if (v < 6) continue;
      const uint8_t kind = heatKind[src][c];
      const uint16_t color = heatColor(v, kind, fg, dim, hot, accent);
      g.fillRect(x0 + (int)col * cw, y0 + (int)(c - 1) * chH, cw - 1, chH - 1, color);
    }
  }
  for (uint8_t c = 1; c <= 13; c++) {
    char lab[4];
    snprintf(lab, sizeof(lab), "%u", (unsigned)c);
    const uint16_t lc = (c == dwellCh) ? 0x0000 : dim;  // black on cyan highlight
    text(g, 2, y0 + (int)(c - 1) * chH, lab, lc, (c == dwellCh) ? accent : bg, 1);
  }

  // Caption: ASCII only - beacon heat / open vs enc + hottest on dwell CH
  text(g, 4, 118, "beacon heat / open vs enc", dim, bg, 1);
  if (hotSsid[0] && hotRssi > -120) {
    snprintf(line, sizeof(line), "CH%u %s %ddBm", (unsigned)hotCh, hotSsid, (int)hotRssi);
    text(g, 4, 126, line, fg, bg, 1);
  } else {
    text(g, 4, 126, "waiting beacons on dwell CH", dim, bg, 1);
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
  text(g, 4, 124, "advert RSSI - not connections", dim, bg, 1);
}

void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 14, "RADIO HELP", accent, bg, 1);
  text(g, 4, 28, "pages: Wi-Fi / Fall / BLE / Sys", fg, bg, 1);
  text(g, 4, 42, "short : next   double : prev", fg, bg, 1);
  text(g, 4, 54, "long  : rescan", fg, bg, 1);
  text(g, 4, 66, "triple: back to charger", fg, bg, 1);
  text(g, 4, 84, "Wi-Fi = beacon APs + heat", dim, bg, 1);
  text(g, 4, 96, "Fall = CH roll beacon heat", dim, bg, 1);
  text(g, 4, 108, "BLE = nearby advertisers", dim, bg, 1);
  text(g, 4, 122, "web: /radio", accent, bg, 1);
}

void drawSys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg, bool wifiUp,
             int8_t wifiRssi, bool mqttOk, bool webOk, uint32_t loopUs, uint16_t loopsPerSec) {
  g.fillScreen(bg);
  char line[48];
  const uint16_t cyan = 0x07FF;
  const uint16_t yellow = 0xFFE0;
  const uint16_t magenta = 0xF81F;

  text(g, 4, 14, "SYSTEM", accent, bg, 1);
  const uint32_t sec = millis() / 1000;
  snprintf(line, sizeof(line), "up %luh%02lum", (unsigned long)(sec / 3600),
           (unsigned long)((sec / 60) % 60));
  text(g, 140, 14, line, dim, bg, 1);

  // Wi-Fi block
  if (wifiUp) {
    char ssidBuf[20];
    truncCopy(ssidBuf, sizeof(ssidBuf), WiFi.SSID().c_str(), 12);
    snprintf(line, sizeof(line), "WiFi %s", ssidBuf[0] ? ssidBuf : "(assoc)");
    text(g, 4, 26, line, fg, bg, 1);
    snprintf(line, sizeof(line), "ch%u", (unsigned)WiFi.channel());
    text(g, 190, 26, line, cyan, bg, 1);

    snprintf(line, sizeof(line), "%d dBm", (int)wifiRssi);
    text(g, 4, 38, line, yellow, bg, 1);
    // RSSI bar: -90..-30
    float rFrac = ((float)wifiRssi + 90.f) / 60.f;
    drawHBar(g, 70, 38, 110, 8, rFrac, cyan, dim);

    snprintf(line, sizeof(line), "IP %s", WiFi.localIP().toString().c_str());
    text(g, 4, 50, line, fg, bg, 1);
  } else {
    text(g, 4, 26, "WiFi down / scanning", dim, bg, 1);
    text(g, 4, 38, "no STA link", dim, bg, 1);
    text(g, 4, 50, "IP --", dim, bg, 1);
  }

  // MQTT + web server (webOk = server listening / started from main)
  snprintf(line, sizeof(line), "MQTT %s", mqttOk ? "up" : "down");
  text(g, 4, 62, line, mqttOk ? cyan : dim, bg, 1);
  snprintf(line, sizeof(line), "WEB %s", webOk ? "up" : "off");
  text(g, 110, 62, line, webOk ? yellow : dim, bg, 1);

  // Heap + min free bars
  const uint32_t heapFree = ESP.getFreeHeap();
  const uint32_t heapMin = ESP.getMinFreeHeap();
  uint32_t heapSize = ESP.getHeapSize();
  if (heapSize < 1024) heapSize = heapFree + heapMin + 1;
  snprintf(line, sizeof(line), "Heap %luk", (unsigned long)(heapFree / 1024));
  text(g, 4, 74, line, fg, bg, 1);
  drawHBar(g, 90, 74, 70, 8, (float)heapFree / (float)heapSize, cyan, dim);
  snprintf(line, sizeof(line), "min %luk", (unsigned long)(heapMin / 1024));
  text(g, 168, 74, line, dim, bg, 1);
  drawHBar(g, 210, 74, 26, 8, (float)heapMin / (float)heapSize, magenta, dim);

  // PSRAM if present
  const uint32_t psramSize = ESP.getPsramSize();
  if (psramSize > 0) {
    const uint32_t psFree = ESP.getFreePsram();
    snprintf(line, sizeof(line), "PSRAM %luk", (unsigned long)(psFree / 1024));
    text(g, 4, 86, line, fg, bg, 1);
    drawHBar(g, 100, 86, 120, 8, (float)psFree / (float)psramSize, yellow, dim);
  } else {
    text(g, 4, 86, "PSRAM none", dim, bg, 1);
  }

  // Loop load: real metric from main (last duration + loops/sec) - no fake CPU%
  if (loopUs < 1000UL) {
    snprintf(line, sizeof(line), "Loop %lu us  %u/s", (unsigned long)loopUs, (unsigned)loopsPerSec);
  } else {
    snprintf(line, sizeof(line), "Loop %lu ms  %u/s", (unsigned long)(loopUs / 1000UL),
             (unsigned)loopsPerSec);
  }
  text(g, 4, 98, line, fg, bg, 1);
  // Bar scales against a soft 20ms "busy" ceiling for duration visualization
  float loadFrac = (float)loopUs / 20000.f;
  drawHBar(g, 150, 98, 80, 8, loadFrac, magenta, dim);

  snprintf(line, sizeof(line), "AP %u  BLE %u", (unsigned)nAps, (unsigned)nBle);
  text(g, 4, 112, line, yellow, bg, 1);
  text(g, 4, 124, "x3 charger  short next", dim, bg, 1);
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

#endif  // HAS_RADIO
