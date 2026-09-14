#include "msc_stick.h"
#include "features.h"
#include "pins.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>
#include <math.h>

#if !HAS_MSC_STICK

namespace MscStick {
bool begin() { return false; }
void tick(uint32_t) {}
bool sdOk() { return false; }
bool usbMounted() { return false; }
float tempC() { return 0.f; }
float readMBps() { return 0.f; }
float writeMBps() { return 0.f; }
uint64_t cardBytes() { return 0; }
uint64_t usedBytes() { return 0; }
uint8_t usedPercent() { return 0; }
const char* linkRateLabel() { return "12Mbps"; }
const float* sparkRead() { return nullptr; }
const float* sparkWrite() { return nullptr; }
uint8_t sparkCount() { return 0; }
void drawDashboard(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
}  // namespace MscStick

#else

#include "USB.h"
#include "USBMSC.h"

namespace MscStick {
namespace {

// Constructed at static-init so MSC interface is enabled before USB.begin()
// (CDC-on-boot path in Arduino core).
USBMSC msc;
SPIClass* sdSpi = nullptr;

bool gSdOk = false;
bool gMscReady = false;
uint32_t gSectorCount = 0;
uint16_t gSectorSize = 512;

volatile uint32_t gReadBytesWindow = 0;
volatile uint32_t gWriteBytesWindow = 0;
uint32_t gLastRwMs = 0;
float gReadMBps = 0.f;
float gWriteMBps = 0.f;
uint32_t gLastActivityMs = 0;

float gSparkR[kSparkLen] = {};
float gSparkW[kSparkLen] = {};
uint8_t gSparkN = 0;

uint64_t gCardBytes = 0;
uint64_t gUsedBytes = 0;
uint8_t gUsedPct = 0;
float gTempC = 0.f;
uint32_t gLastCapMs = 0;
uint32_t gLastTempMs = 0;

static void gfxText(Adafruit_GFX& g, int16_t x, int16_t y, const char* s, uint16_t fg,
                    uint16_t bg, uint8_t size = 1, bool centerX = false) {
  g.setTextSize(size);
  g.setTextColor(fg, bg);
  g.setTextWrap(false);
  if (centerX) x = (int16_t)(x - (int)strlen(s) * 6 * size / 2);
  g.setCursor(x, y);
  g.print(s);
}

static void noteBytes(volatile uint32_t& counter, uint32_t n) {
  counter += n;
  gLastActivityMs = millis();
}

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!gSdOk || !buffer || !bufsize) return -1;
  uint8_t* out = static_cast<uint8_t*>(buffer);
  uint32_t done = 0;
  uint32_t sectorOff = offset;
  uint32_t sector = lba;

  while (done < bufsize) {
    uint8_t blk[512];
    if (gSectorSize != 512) return -1;
    if (!SD.readRAW(blk, sector)) return -1;
    const uint32_t take = min((uint32_t)(gSectorSize - sectorOff), bufsize - done);
    memcpy(out + done, blk + sectorOff, take);
    done += take;
    sectorOff = 0;
    sector++;
  }
  noteBytes(gReadBytesWindow, bufsize);
  return (int32_t)bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (!gSdOk || !buffer || !bufsize) return -1;
  uint32_t done = 0;
  uint32_t sectorOff = offset;
  uint32_t sector = lba;

  while (done < bufsize) {
    uint8_t blk[512];
    if (gSectorSize != 512) return -1;
    if (sectorOff != 0 || (bufsize - done) < gSectorSize) {
      if (!SD.readRAW(blk, sector)) return -1;
    }
    const uint32_t take = min((uint32_t)(gSectorSize - sectorOff), bufsize - done);
    memcpy(blk + sectorOff, buffer + done, take);
    if (!SD.writeRAW(blk, sector)) return -1;
    done += take;
    sectorOff = 0;
    sector++;
  }
  noteBytes(gWriteBytesWindow, bufsize);
  return (int32_t)bufsize;
}

static bool onStartStop(uint8_t /*power*/, bool start, bool load_eject) {
  if (load_eject && !start) {
    msc.mediaPresent(false);
  } else if (start) {
    msc.mediaPresent(gSdOk);
  }
  return true;
}

static void refreshCapacity() {
  if (!gSdOk) {
    gCardBytes = 0;
    gUsedBytes = 0;
    gUsedPct = 0;
    return;
  }
  gCardBytes = SD.cardSize();
  gUsedBytes = SD.usedBytes();
  if (gCardBytes == 0) {
    gUsedPct = 0;
  } else {
    uint64_t pct = (gUsedBytes * 100ULL) / gCardBytes;
    if (pct > 100) pct = 100;
    gUsedPct = (uint8_t)pct;
  }
}

static void pushSpark(float r, float w) {
  if (gSparkN < kSparkLen) {
    gSparkR[gSparkN] = r;
    gSparkW[gSparkN] = w;
    gSparkN++;
  } else {
    memmove(gSparkR, gSparkR + 1, (kSparkLen - 1) * sizeof(float));
    memmove(gSparkW, gSparkW + 1, (kSparkLen - 1) * sizeof(float));
    gSparkR[kSparkLen - 1] = r;
    gSparkW[kSparkLen - 1] = w;
  }
}

static void formatSize(uint64_t bytes, char* out, size_t n) {
  if (bytes >= (1ULL << 30)) {
    snprintf(out, n, "%lluGB", (unsigned long long)((bytes + (1ULL << 29)) >> 30));
  } else if (bytes >= (1ULL << 20)) {
    snprintf(out, n, "%lluMB", (unsigned long long)((bytes + (1ULL << 19)) >> 20));
  } else if (bytes >= (1ULL << 10)) {
    snprintf(out, n, "%lluKB", (unsigned long long)((bytes + (1ULL << 9)) >> 10));
  } else {
    snprintf(out, n, "%lluB", (unsigned long long)bytes);
  }
}

static void drawTempGauge(Adafruit_GFX& g, int16_t cx, int16_t cy, int16_t r, float temp,
                          uint16_t cyan, uint16_t orange, uint16_t fg, uint16_t bg) {
  // Arc from ~225° to ~315° (bottom-open gauge), filled by temp 20..70°C.
  const float tNorm = constrain((temp - 20.f) / 50.f, 0.f, 1.f);
  const int segs = 24;
  for (int i = 0; i < segs; i++) {
    const float a0 = 3.14159265f * 0.75f + (i / (float)segs) * 3.14159265f * 1.5f;
    const float a1 = 3.14159265f * 0.75f + ((i + 1) / (float)segs) * 3.14159265f * 1.5f;
    const bool hot = (i / (float)segs) >= 0.65f;
    const bool lit = (i / (float)segs) <= tNorm;
    const uint16_t col = lit ? (hot ? orange : cyan) : (uint16_t)0x2104;
    const int16_t x0 = cx + (int16_t)(cosf(a0) * r);
    const int16_t y0 = cy + (int16_t)(sinf(a0) * r);
    const int16_t x1 = cx + (int16_t)(cosf(a1) * r);
    const int16_t y1 = cy + (int16_t)(sinf(a1) * r);
    g.drawLine(x0, y0, x1, y1, col);
    g.drawLine(x0, y0 + 1, x1, y1 + 1, col);
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "%.0f", temp);
  gfxText(g, cx, cy - 8, buf, fg, bg, 2, true);
  gfxText(g, cx, cy + 10, "C", cyan, bg, 1, true);
}

static void drawSparkline(Adafruit_GFX& g, int16_t x, int16_t y, int16_t w, int16_t h,
                          const float* data, uint8_t n, uint16_t lineCol, uint16_t fillCol) {
  g.drawRect(x, y, w, h, 0x2945);
  if (n < 2) return;
  float mx = 0.05f;
  for (uint8_t i = 0; i < n; i++) {
    if (data[i] > mx) mx = data[i];
  }
  int16_t prevX = x + 1;
  int16_t prevY = y + h - 2;
  for (uint8_t i = 0; i < n; i++) {
    const int16_t px = x + 1 + (int16_t)((i * (w - 3)) / (n - 1));
    const float v = constrain(data[i] / mx, 0.f, 1.f);
    const int16_t py = y + h - 2 - (int16_t)(v * (h - 4));
    if (i > 0) {
      g.drawLine(prevX, prevY, px, py, lineCol);
      // translucent-ish fill: vertical dots under the line
      for (int16_t fy = py; fy < y + h - 1; fy += 2) g.drawPixel(px, fy, fillCol);
    }
    prevX = px;
    prevY = py;
  }
}

static void drawBadge(Adafruit_GFX& g, int16_t x, int16_t y, const char* label, uint16_t fg,
                      uint16_t bg, uint16_t border) {
  const int tw = (int)strlen(label) * 6 + 14;
  g.fillRoundRect(x, y, tw, 14, 3, bg);
  g.drawRoundRect(x, y, tw, 14, 3, border);
  // lightning bolt cue
  g.drawLine(x + 3, y + 10, x + 6, y + 3, border);
  g.drawLine(x + 6, y + 3, x + 5, y + 7, border);
  g.drawLine(x + 5, y + 7, x + 8, y + 2, border);
  gfxText(g, x + 10, y + 3, label, fg, bg, 1);
}

static void drawPctRing(Adafruit_GFX& g, int16_t cx, int16_t cy, int16_t r, uint8_t pct,
                        uint16_t on, uint16_t off) {
  g.drawCircle(cx, cy, r, off);
  const int segs = 20;
  const int lit = (pct * segs + 50) / 100;
  for (int i = 0; i < lit; i++) {
    const float a = -1.5708f + (i / (float)segs) * 6.2832f;
    const int16_t x = cx + (int16_t)(cosf(a) * r);
    const int16_t y = cy + (int16_t)(sinf(a) * r);
    g.fillCircle(x, y, 1, on);
  }
}

}  // namespace

bool begin() {
  sdSpi = new SPIClass(HSPI);
  sdSpi->begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  gSdOk = SD.begin(PIN_SD_CS, *sdSpi, 20000000);
  Serial.println(gSdOk ? "MSC stick: SD OK" : "MSC stick: SD missing");

  if (gSdOk) {
    gSectorSize = (uint16_t)SD.sectorSize();
    if (gSectorSize == 0) gSectorSize = 512;
    gSectorCount = (uint32_t)SD.numSectors();
    refreshCapacity();
  }

  msc.vendorID("GEEK");
  msc.productID("TF Stick");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(gSdOk);

  if (gSdOk && gSectorCount > 0) {
    gMscReady = msc.begin(gSectorCount, gSectorSize);
  } else {
    // Still advertise a tiny empty disk so composite stays stable; mediaPresent=false.
    gMscReady = msc.begin(16, 512);
    msc.mediaPresent(false);
  }
  Serial.printf("MSC stick: USB MSC %s (sectors=%lu size=%u)\n", gMscReady ? "ready" : "FAIL",
                (unsigned long)gSectorCount, (unsigned)gSectorSize);

  gTempC = temperatureRead();
  gLastRwMs = millis();
  gLastCapMs = millis();
  gLastTempMs = millis();
  return gMscReady;
}

void tick(uint32_t now) {
  if (now - gLastRwMs >= 250) {
    const float dt = (now - gLastRwMs) / 1000.f;
    const uint32_t rb = gReadBytesWindow;
    const uint32_t wb = gWriteBytesWindow;
    gReadBytesWindow = 0;
    gWriteBytesWindow = 0;
    gReadMBps = dt > 0.f ? (rb / (1024.f * 1024.f)) / dt : 0.f;
    gWriteMBps = dt > 0.f ? (wb / (1024.f * 1024.f)) / dt : 0.f;
    // Idle decay to zeros when host quiet
    if (rb == 0 && wb == 0) {
      gReadMBps *= 0.4f;
      gWriteMBps *= 0.4f;
      if (gReadMBps < 0.01f) gReadMBps = 0.f;
      if (gWriteMBps < 0.01f) gWriteMBps = 0.f;
    }
    pushSpark(gReadMBps, gWriteMBps);
    gLastRwMs = now;
  }

  if (now - gLastTempMs >= 1000) {
    gTempC = temperatureRead();
    gLastTempMs = now;
  }

  // Capacity refresh when idle (avoid FS vs host races during transfer)
  if (now - gLastCapMs >= 3000) {
    gLastCapMs = now;
    if (gSdOk && (now - gLastActivityMs) > 800) refreshCapacity();
  }
}

bool sdOk() { return gSdOk; }
bool usbMounted() { return gSdOk && (millis() - gLastActivityMs) < 1500; }
float tempC() { return gTempC; }
float readMBps() { return gReadMBps; }
float writeMBps() { return gWriteMBps; }
uint64_t cardBytes() { return gCardBytes; }
uint64_t usedBytes() { return gUsedBytes; }
uint8_t usedPercent() { return gUsedPct; }
const char* linkRateLabel() { return "12Mbps"; }  // ESP32-S3 native USB Full Speed
const float* sparkRead() { return gSparkR; }
const float* sparkWrite() { return gSparkW; }
uint8_t sparkCount() { return gSparkN; }

void drawDashboard(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t warn,
                   uint16_t bg) {
  const int16_t W = g.width();

  if (!gSdOk) {
    gfxText(g, W / 2, 40, "NO SD CARD", warn, bg, 2, true);
    gfxText(g, W / 2, 70, "Insert FAT TF", dim, bg, 1, true);
    gfxText(g, W / 2, 86, "then replug USB", dim, bg, 1, true);
    drawBadge(g, W - 78, 110, linkRateLabel(), fg, bg, accent);
    return;
  }

  // Temp gauge (top-center)
  drawTempGauge(g, W / 2, 42, 28, gTempC, accent, warn, fg, bg);

  // R / W speeds + combined sparkline
  char line[28];
  g.fillRoundRect(4, 72, 14, 12, 2, 0xFFE0);  // yellow R badge
  gfxText(g, 7, 74, "R", 0x0000, 0xFFE0, 1);
  snprintf(line, sizeof(line), "%.2f MB/s", gReadMBps);
  gfxText(g, 22, 74, line, fg, bg, 1);

  g.fillRoundRect(4, 88, 14, 12, 2, warn);  // orange W badge
  gfxText(g, 7, 90, "W", 0x0000, warn, 1);
  snprintf(line, sizeof(line), "%.2f MB/s", gWriteMBps);
  gfxText(g, 22, 90, line, fg, bg, 1);

  // Combined sparkline (max of R/W history as one line for clarity)
  float combo[kSparkLen];
  for (uint8_t i = 0; i < gSparkN; i++) {
    combo[i] = gSparkR[i] > gSparkW[i] ? gSparkR[i] : gSparkW[i];
  }
  drawSparkline(g, 118, 72, W - 122, 30, combo, gSparkN, accent, 0x02AA);

  // Capacity + link badge
  char cap[16];
  formatSize(gCardBytes, cap, sizeof(cap));
  gfxText(g, 4, 112, cap, 0xFFE0, bg, 1);
  drawPctRing(g, 70, 118, 6, gUsedPct, 0x07E0, dim);
  snprintf(line, sizeof(line), "%u%%", (unsigned)gUsedPct);
  gfxText(g, 80, 112, line, 0x07E0, bg, 1);

  drawBadge(g, W - 78, 110, linkRateLabel(), fg, bg, warn);

  // Vertical brand hint (tiny)
  gfxText(g, W - 8, 16, "G", dim, bg, 1);
  gfxText(g, W - 8, 26, "E", dim, bg, 1);
  gfxText(g, W - 8, 36, "E", dim, bg, 1);
  gfxText(g, W - 8, 46, "K", dim, bg, 1);
}

}  // namespace MscStick

#endif  // HAS_MSC_STICK
