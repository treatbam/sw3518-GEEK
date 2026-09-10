#include "session_store.h"
#include "app_state.h"

#include <Preferences.h>
#include <string.h>

static Preferences prefs;
static uint32_t lastSaveMs = 0;
static constexpr uint32_t kSaveMs = 5000;
static constexpr uint32_t kMagic = 0x4745454Bu;  // GEEK
static constexpr uint16_t kVer = 2;

struct PersistedSession {
  uint32_t magic;
  uint16_t version;
  uint16_t histCount;
  uint32_t chargedMs;
  uint32_t histPeriodMs;
  double mwh;
  float peakW;
  float peakA;
  PortStats portC;
  PortStats portA;
  uint16_t peakVoutMv;
  uint16_t _pad;
  float histC[Session::kHistMax];
  float histA[Session::kHistMax];
};

static void blobToSession(const PersistedSession& blob, Session& s) {
  s = Session{};
  s.chargedMs = blob.chargedMs;
  s.mwh = blob.mwh;
  s.peakW = blob.peakW;
  s.peakA = blob.peakA;
  s.portC = blob.portC;
  s.portA = blob.portA;
  s.peakVoutMv = blob.peakVoutMv;
  s.histCount = blob.histCount;
  if (s.histCount > Session::kHistMax) s.histCount = Session::kHistMax;
  s.histPeriodMs = blob.histPeriodMs ? blob.histPeriodMs : Session::kHistPeriodInitMs;
  memcpy(s.histC, blob.histC, sizeof(s.histC));
  memcpy(s.histA, blob.histA, sizeof(s.histA));
  s.adoptPersisted();
}

void sessionCaptureSaved() {
  app.savedOk = app.session.hasData();
  if (!app.savedOk) return;
  app.saved = app.session;
}

static void captureSavedFromBlob(const PersistedSession& blob) {
  blobToSession(blob, app.saved);
  app.savedOk = app.saved.hasData();
}

void sessionStoreLoad() {
  if (!prefs.begin("geek-sess", true)) return;
  PersistedSession blob = {};
  const size_t n = prefs.getBytes("snap", &blob, sizeof(blob));
  prefs.end();
  if (n != sizeof(blob) || blob.magic != kMagic || blob.version != kVer) {
    Serial.println("No persisted session");
    return;
  }
  blobToSession(blob, app.session);
  captureSavedFromBlob(blob);
  Serial.printf("Session restored: %.1f mWh  peak %.1f W  hist %u\n", app.session.mwh,
                app.session.peakW, (unsigned)app.session.histCount);
}

void sessionStoreTick(uint32_t now, bool force) {
  if (!force) {
    if (!app.session.dirty) return;
    if (now - lastSaveMs < kSaveMs) return;
  }
  PersistedSession blob = {};
  blob.magic = kMagic;
  blob.version = kVer;
  blob.histCount = (uint16_t)app.session.histCount;
  blob.chargedMs = app.session.chargedMs;
  blob.histPeriodMs = app.session.histPeriodMs;
  blob.mwh = app.session.mwh;
  blob.peakW = app.session.peakW;
  blob.peakA = app.session.peakA;
  blob.portC = app.session.portC;
  blob.portA = app.session.portA;
  blob.peakVoutMv = app.session.peakVoutMv;
  memcpy(blob.histC, app.session.histC, sizeof(blob.histC));
  memcpy(blob.histA, app.session.histA, sizeof(blob.histA));

  if (!prefs.begin("geek-sess", false)) {
    Serial.println("NVS session open failed");
    return;
  }
  const size_t n = prefs.putBytes("snap", &blob, sizeof(blob));
  prefs.end();
  if (n == sizeof(blob)) {
    app.session.dirty = false;
    lastSaveMs = now;
    captureSavedFromBlob(blob);
    Serial.printf("Session saved (%u B)\n", (unsigned)n);
  } else {
    Serial.println("Session save short write");
  }
}

void sessionStoreErase() {
  if (!prefs.begin("geek-sess", false)) return;
  prefs.clear();
  prefs.end();
  app.session.dirty = false;
  Serial.println("Session NVS erased");
}
