#include "radio_tools.h"
#include <string.h>

namespace RadioTools {
namespace {

ApRow aps[kMaxAps];
uint8_t nAps = 0;
bool scanBusy = false;
bool active = false;
uint32_t lastScan = 0;
uint32_t nextScanDue = 0;
static constexpr uint32_t kScanPeriodMs = 6000;

// heat[col][ch] — rolling columns of channel activity 0..255
uint8_t heat[kHeatCols][kChannels];
size_t heatHead = 0;

void pushHeatFromScan() {
  uint16_t score[kChannels] = {};
  for (uint8_t i = 0; i < nAps; i++) {
    const uint8_t ch = aps[i].channel;
    if (ch == 0 || ch >= kChannels) continue;
    // Stronger RSSI → hotter; floor at -90
    int s = (int)aps[i].rssi + 90;
    if (s < 0) s = 0;
    if (s > 60) s = 60;
    score[ch] = (uint16_t)(score[ch] + (uint16_t)s + 8);
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

void ingestScan() {
  const int16_t n = WiFi.scanComplete();
  if (n < 0) return;  // still running or failed (-2 = running)
  scanBusy = false;
  lastScan = millis();
  nAps = 0;
  if (n == 0) {
    WiFi.scanDelete();
    pushHeatFromScan();
    return;
  }
  // Take strongest APs
  for (int i = 0; i < n && nAps < kMaxAps; i++) {
    // WiFi.SSID/RSSI already sorted? Not always — pick by walking sorted indices
  }
  // Build index list sorted by RSSI desc (simple selection of top k)
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

}  // namespace

void begin() {
  memset(heat, 0, sizeof(heat));
  heatHead = 0;
  nAps = 0;
}

void enter() {
  active = true;
  nextScanDue = 0;  // scan ASAP
}

void leave() {
  active = false;
  if (scanBusy) {
    WiFi.scanDelete();
    scanBusy = false;
  }
}

void requestScan() { nextScanDue = 0; }

void tick(uint32_t now) {
  if (!active) return;
  if (scanBusy) {
    ingestScan();
    return;
  }
  if (now >= nextScanDue) {
    // Async scan; keep STA association (ESP32 supports scan while connected)
    const int16_t r = WiFi.scanNetworks(/*async=*/true, /*hidden=*/true);
    if (r == WIFI_SCAN_RUNNING || r >= 0) {
      scanBusy = true;
      nextScanDue = now + kScanPeriodMs;
    } else {
      nextScanDue = now + 2000;
    }
  }
}

uint8_t apCount() { return nAps; }
const ApRow* apAt(uint8_t i) { return (i < nAps) ? &aps[i] : nullptr; }
bool scanning() { return scanBusy; }
uint32_t lastScanMs() { return lastScan; }

static void text(Adafruit_GFX& g, int x, int y, const char* s, uint16_t c, uint16_t bg, uint8_t size = 1) {
  g.setTextSize(size);
  g.setTextColor(c, bg);
  g.setCursor(x, y);
  g.print(s);
}

void drawApList(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t bar, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 2, "WI-FI SCAN", 0x07FF, bg, 1);
  char line[40];
  if (scanBusy) snprintf(line, sizeof(line), "scanning...");
  else snprintf(line, sizeof(line), "%u APs", (unsigned)nAps);
  text(g, 160, 2, line, dim, bg, 1);

  if (nAps == 0) {
    text(g, 4, 40, scanBusy ? "listening for beacons" : "no APs yet — wait", dim, bg, 1);
    text(g, 4, 118, "long = rescan   x3 = charger", dim, bg, 1);
    return;
  }

  for (uint8_t i = 0; i < nAps && i < 6; i++) {
    const ApRow& a = aps[i];
    const int y = 16 + i * 18;
    snprintf(line, sizeof(line), "ch%u", (unsigned)a.channel);
    text(g, 4, y, line, dim, bg, 1);
    text(g, 34, y, a.ssid, fg, bg, 1);
    // RSSI bar: -90..-30 → 0..100px
    int w = (int)a.rssi + 90;
    if (w < 0) w = 0;
    if (w > 60) w = 60;
    w = w * 100 / 60;
    g.fillRect(34, y + 10, w, 4, bar);
    g.drawRect(34, y + 10, 100, 4, dim);
    snprintf(line, sizeof(line), "%d", (int)a.rssi);
    text(g, 150, y, line, fg, bg, 1);
  }
  text(g, 4, 124, "short next  long rescan  x3 exit", dim, bg, 1);
}

void drawChannelHeat(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t hot, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 2, "CHANNEL HEAT", 0x07FF, bg, 1);
  text(g, 140, 2, scanBusy ? "scan..." : "AP airtime", dim, bg, 1);

  // Draw scrolling heatmap: x = time, y = channel 1..13
  const int x0 = 20, y0 = 18, cw = 4, ch = 7;
  for (size_t col = 0; col < kHeatCols; col++) {
    const size_t src = (heatHead + col) % kHeatCols;
    for (uint8_t c = 1; c <= 13; c++) {
      const uint8_t v = heat[src][c];
      if (v < 8) continue;
      // blend toward hot color by intensity (simple threshold colors)
      uint16_t color = dim;
      if (v > 40) color = fg;
      if (v > 120) color = hot;
      g.fillRect(x0 + (int)col * cw, y0 + (int)(c - 1) * ch, cw - 1, ch - 1, color);
    }
  }
  // Channel labels
  for (uint8_t c = 1; c <= 13; c += 3) {
    char lab[4];
    snprintf(lab, sizeof(lab), "%u", (unsigned)c);
    text(g, 4, y0 + (int)(c - 1) * ch, lab, dim, bg, 1);
  }
  text(g, 4, 118, "from beacon RSSI — not packet sniff", dim, bg, 1);
}

void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  g.fillScreen(bg);
  text(g, 4, 2, "CONTROLS", accent, bg, 1);
  text(g, 4, 18, "BOOT short : next page", fg, bg, 1);
  text(g, 4, 32, "BOOT double: Session / prev", fg, bg, 1);
  text(g, 4, 46, "BOOT triple: Charger <-> Radio", fg, bg, 1);
  text(g, 4, 60, "BOOT long  : clear / rescan", fg, bg, 1);
  text(g, 4, 80, "Radio = Wi-Fi scan tools", dim, bg, 1);
  text(g, 4, 94, "Two side buttons later", dim, bg, 1);
  text(g, 4, 118, "web: /radio  /help", accent, bg, 1);
}

void drawSys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg,
             bool wifiUp, int8_t wifiRssi) {
  g.fillScreen(bg);
  text(g, 4, 2, "SYSTEM", accent, bg, 1);
  char line[48];
  const uint32_t sec = millis() / 1000;
  snprintf(line, sizeof(line), "up %luh %lum", (unsigned long)(sec / 3600),
           (unsigned long)((sec / 60) % 60));
  text(g, 4, 20, line, fg, bg, 1);
  snprintf(line, sizeof(line), "heap %u", (unsigned)ESP.getFreeHeap());
  text(g, 4, 36, line, fg, bg, 1);
  snprintf(line, sizeof(line), "cpu %u MHz", (unsigned)(ESP.getCpuFreqMHz()));
  text(g, 4, 52, line, fg, bg, 1);
  if (wifiUp) {
    snprintf(line, sizeof(line), "wifi %s  %d dBm", WiFi.localIP().toString().c_str(), (int)wifiRssi);
  } else {
    snprintf(line, sizeof(line), "wifi down");
  }
  text(g, 4, 68, line, fg, bg, 1);
  snprintf(line, sizeof(line), "SDK %s", ESP.getSdkVersion());
  text(g, 4, 90, line, dim, bg, 1);
  text(g, 4, 118, "x3 charger   short next", dim, bg, 1);
}

size_t jsonStatus(char* out, size_t n) {
  size_t o = 0;
  auto append = [&](const char* s) {
    while (*s && o + 1 < n) out[o++] = *s++;
  };
  append("{\"scanning\":");
  append(scanBusy ? "true" : "false");
  append(",\"aps\":[");
  for (uint8_t i = 0; i < nAps; i++) {
    if (i) append(",");
    char safe[24];
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
  append("]}");
  out[o] = 0;
  return o;
}

}  // namespace RadioTools
