#include "ui.h"
#include "net.h"
#include "radio_tools.h"
#include "hid_tools.h"
#include "pins.h"
#include "session.h"

#include <math.h>
#include <string.h>

struct Rect {
  float x, y, w, h;
};

static Rect lerpRect(const Rect& a, const Rect& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t,
          a.h + (b.h - a.h) * t};
}

static Rect sparkMiniC() { return {4, 106, 110, 26}; }
static Rect sparkMiniA() { return {126, 106, 110, 26}; }
static Rect sparkPort() { return {4, 72, 232, 58}; }

void formatDuration(uint32_t ms, char* out, size_t n) {
  const uint32_t s = ms / 1000, m = s / 60, h = m / 60;
  if (h > 0) snprintf(out, n, "%luh%02lum", (unsigned long)h, (unsigned long)(m % 60));
  else if (m > 0) snprintf(out, n, "%lum%02lus", (unsigned long)m, (unsigned long)(s % 60));
  else snprintf(out, n, "%lus", (unsigned long)s);
}

void uiBeginPanel() { app.tft.beginPanel(); }

void uiApplyBacklight() {
  if (app.blLevel >= kBlFull) digitalWrite(PIN_TFT_BL, HIGH);
  else analogWrite(PIN_TFT_BL, app.blLevel);
}

void uiTouchActivity() {
  app.lastActivityMs = millis();
  if (app.nightDim) {
    app.nightDim = false;
    app.blLevel = kBlFull;
    uiApplyBacklight();
  }
}

void uiShowModeToast(const char* label) {
  strncpy(app.modeToast, label, sizeof(app.modeToast) - 1);
  app.modeToast[sizeof(app.modeToast) - 1] = 0;
  app.modeToastUntil = millis() + 900;
}

void uiStartZoom(Anim::Kind kind, Page from, Page to) {
  app.anim.kind = kind;
  app.anim.from = from;
  app.anim.to = to;
  app.anim.startMs = millis();
}

void uiResetHistFaceTimer(uint32_t now) {
  if (now == 0) now = millis();
  app.histFaceSinceMs = now;
}

void uiTickHistFace(uint32_t now) {
  if (app.mode != Mode::Charger || app.page != Page::History) return;
  if (app.histFaceSinceMs == 0) app.histFaceSinceMs = now;
  static constexpr uint32_t kSessionMs = 60000;
  static constexpr uint32_t kSavedMs = 15000;
  const uint32_t dwell = (app.histFace == HistFace::Saved) ? kSavedMs : kSessionMs;
  if (now - app.histFaceSinceMs < dwell) return;
  if (app.histFace == HistFace::Session) {
    if (app.savedOk) app.histFace = HistFace::Saved;
  } else {
    app.histFace = HistFace::Session;
  }
  app.histFaceSinceMs = now;
}

void uiFinishAnim(uint32_t now) {
  if (!app.anim.busy()) return;
  if (app.anim.rawT(now) < 1.f) return;
  app.page = app.anim.to;
  if (app.page == Page::History) uiResetHistFaceTimer();
  if (app.anim.kind == Anim::ZoomOut) {
    if (app.anim.from == Page::UsbC) app.seenUsbC = true;
    if (app.anim.from == Page::UsbA) app.seenUsbA = true;
    if (app.seenUsbC && app.seenUsbA && app.anim.from == Page::UsbA) {
      app.ipShowUntilMs = now + 10000;
      app.seenUsbC = false;
      app.seenUsbA = false;
    }
    app.nextFromMain = (app.nextFromMain + 1) % 3;
  }
  app.anim.kind = Anim::Idle;
}

void uiPush() { app.tft.push(app.frame); }

static uint32_t histSpanMs(const Session& s) {
  if (s.histCount < 2) return s.histPeriodMs;
  return (uint32_t)((s.histCount - 1) * s.histPeriodMs);
}

static void drawSparkline(Adafruit_GFX& g, int x, int y, int w, int h, const float* data,
                          uint16_t color, size_t count) {
  g.drawRect(x, y, w, h, COL_DARKGREY);
  if (count < 2 || w < 4 || h < 4) return;
  float mx = 0.1f;
  for (size_t i = 0; i < count; i++) {
    if (data[i] > mx) mx = data[i];
  }
  int prevX = x + 1, prevY = y + h - 2;
  const size_t denom = count > 1 ? count - 1 : 1;
  for (size_t i = 0; i < count; i++) {
    const int px = x + 1 + (int)((w - 3) * i / denom);
    const int py = y + h - 2 - (int)((h - 4) * (data[i] / mx));
    if (i > 0) g.drawLine(prevX, prevY, px, py, color);
    prevX = px;
    prevY = py;
  }
}

static void drawConnIcons(Adafruit_GFX& g, int16_t rightX, int16_t y, bool withIp) {
  const uint32_t now = millis();
  const int16_t xWeb = rightX - 12;
  const int16_t xMqtt = xWeb - 14;
  const int16_t xWifi = xMqtt - 16;
  drawWifiIcon(g, xWifi, y, netWifiBars(), COL_CYAN, COL_DIM);
  drawMqttIcon(g, xMqtt, y, netMqttOk(), COL_GREEN, COL_DIM);
  drawWebIcon(g, xWeb, y, netWebActive(now), COL_ORANGE, COL_DIM);
  if (withIp && now < app.ipShowUntilMs) {
    char ip[20];
    netIpText(ip, sizeof(ip));
    gfxText(g, xWifi - 4, y + 1, ip, COL_CYAN, COL_BLACK, 1, false, true);
  }
}

static void drawUnlinkedIcon(Adafruit_GFX& g, int x, int y) {
  g.drawRect(x, y, 10, 8, COL_ORANGE);
  g.fillRect(x + 1, y + 1, 8, 6, COL_BLACK);
  g.drawFastVLine(x + 2, y - 1, 2, COL_ORANGE);
  g.drawFastVLine(x + 5, y - 1, 2, COL_ORANGE);
  g.drawFastVLine(x + 8, y - 1, 2, COL_ORANGE);
  g.drawFastVLine(x + 2, y + 7, 2, COL_ORANGE);
  g.drawFastVLine(x + 5, y + 7, 2, COL_ORANGE);
  g.drawFastVLine(x + 8, y + 7, 2, COL_ORANGE);
  g.drawLine(x, y + 7, x + 9, y, COL_ORANGE);
  g.drawLine(x, y + 8, x + 9, y + 1, COL_ORANGE);
}

static void drawStatusBar(bool withIp) {
  auto& frame = app.frame;
  const int w = frame.width();
  frame.fillRect(0, 0, w, kStatusBarH, COL_BLACK);
  frame.drawFastHLine(0, kStatusBarH - 1, w, COL_DIM);

  const bool radio = (app.mode == Mode::Radio);
  const bool hid = (app.mode == Mode::Hid);
  const char* modeTag = radio ? "RAD" : (hid ? "HID" : "CHG");
  const uint16_t modeCol = radio ? COL_MAGENTA : (hid ? COL_YELLOW : COL_CYAN);
  gfxText(frame, 2, 2, modeTag, modeCol, COL_BLACK, 1);

  const char* crumbs[5];
  int n = 0;
  int active = 0;
  if (radio) {
    crumbs[n++] = "WIFI";
    crumbs[n++] = "FALL";
    crumbs[n++] = "BLE";
    crumbs[n++] = "SYS";
    crumbs[n++] = "HELP";
    active = (int)app.radioPage;
  } else if (hid) {
    crumbs[n++] = "STAT";
    crumbs[n++] = "KEYS";
    crumbs[n++] = "MSE";
    crumbs[n++] = "MAC";
    crumbs[n++] = "HELP";
    active = (int)app.hidPage;
  } else {
    crumbs[n++] = "MAIN";
    crumbs[n++] = "C";
    crumbs[n++] = "A";
    crumbs[n++] = (app.page == Page::History && app.histFace == HistFace::Saved && app.savedOk)
                      ? "SAV"
                      : "SES";
    if (app.page == Page::Main) active = 0;
    else if (app.page == Page::UsbC) active = 1;
    else if (app.page == Page::UsbA) active = 2;
    else active = 3;
  }
  if (active < 0) active = 0;
  if (active >= n) active = n - 1;

  int x = 28;
  for (int i = 0; i < n; i++) {
    const bool on = (i == active);
    gfxText(frame, x, 2, crumbs[i], on ? COL_WHITE : COL_DARKGREY, COL_BLACK, 1);
    const int tw = (int)strlen(crumbs[i]) * 6;
    if (on) {
      frame.drawFastHLine(x, 10, tw, radio ? COL_MAGENTA : (hid ? COL_YELLOW : COL_CYAN));
    }
    x += tw + 6;
    if (i + 1 < n) gfxText(frame, x - 5, 2, ".", COL_DIM, COL_BLACK, 1);
  }

  if (!app.charger.present()) {
    gfxText(frame, 2, 2, modeTag, COL_ORANGE, COL_BLACK, 1);
    const int16_t xWifi = (w - 2) - 12 - 14 - 16;
    drawUnlinkedIcon(frame, xWifi - 14, 2);
  }
  drawConnIcons(frame, w - 2, 1, withIp);
}

static void drawLoadShareBar(int y) {
  auto& frame = app.frame;
  const float pc = app.snap.power_c_w;
  const float pa = app.snap.power_a_w;
  const float tot = pc + pa;
  const int w = frame.width() - 8;
  frame.drawRect(4, y, w, 6, COL_DARKGREY);
  if (tot < 0.05f) {
    gfxText(frame, 4, y - 10, "C/A share --", COL_DARKGREY, COL_BLACK, 1);
    return;
  }
  int wc = (int)(w * (pc / tot) + 0.5f);
  if (wc < 0) wc = 0;
  if (wc > w) wc = w;
  if (wc > 0) frame.fillRect(4, y, wc, 6, COL_YELLOW);
  if (wc < w) frame.fillRect(4 + wc, y, w - wc, 6, COL_MAGENTA);
  char buf[28];
  snprintf(buf, sizeof(buf), "C %.0f%%  A %.0f%%", 100.f * pc / tot, 100.f * pa / tot);
  gfxText(frame, 4, y - 10, buf, COL_LIGHTGREY, COL_BLACK, 1);
}

static void drawMainChrome() {
  auto& frame = app.frame;
  const int w = frame.width();
  drawStatusBar(true);

  const bool charging =
      app.snap.ia_ma > Session::kLoadMa || app.snap.ic_ma > Session::kLoadMa;
  const bool flash = millis() < app.protoFlashUntil;
  const char* proto = SW3518::protocolName(app.snap.protocol);
  gfxText(frame, 4, 14, charging ? "LIVE" : "IDLE", charging ? COL_GREEN : COL_LIGHTGREY, COL_BLACK,
          1);
  if (flash) frame.fillRect(w / 2 - 50, 14, 100, 12, COL_YELLOW);
  gfxText(frame, w / 2, 14, proto, flash ? COL_BLACK : COL_YELLOW, flash ? COL_YELLOW : COL_BLACK, 1,
          true);

  char buf[40];
  snprintf(buf, sizeof(buf), "%.1fW", app.snap.power_total_w);
  gfxText(frame, w / 2, 28, buf, COL_WHITE, COL_BLACK, 2, true);

  snprintf(buf, sizeof(buf), "in %.1fV", app.snap.vin_mv / 1000.0f);
  gfxText(frame, 4, 52, buf, COL_LIGHTGREY, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "out %.2fV", app.snap.vout_mv / 1000.0f);
  gfxText(frame, w / 2, 52, buf, COL_LIGHTGREY, COL_BLACK, 1);

  gfxText(frame, 4, 66, "C", COL_YELLOW, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA", app.snap.ic_ma / 1000.0f);
  gfxText(frame, 14, 66, buf, COL_WHITE, COL_BLACK, 1);
  gfxText(frame, w / 2, 66, "A", COL_MAGENTA, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA", app.snap.ia_ma / 1000.0f);
  gfxText(frame, w / 2 + 10, 66, buf, COL_WHITE, COL_BLACK, 1);

  drawLoadShareBar(86);

  if (app.session.hasData()) {
    char dur[16];
    formatDuration(app.session.chargedMs, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "%s  pk %.0fW avg %.0fW  %.0fmWh", dur, app.session.peakW,
             app.session.avgW(), app.session.mwh);
  } else {
    snprintf(buf, sizeof(buf), "idle  long=clear  x3=mode");
  }
  gfxText(frame, 4, 96, buf, COL_LIGHTGREY, COL_BLACK, 1);
}

static void drawPortChrome(bool usbC, float alpha) {
  if (alpha < 0.35f) return;
  auto& frame = app.frame;
  const int w = frame.width();
  drawStatusBar(true);
  const uint16_t accent = usbC ? COL_YELLOW : COL_MAGENTA;
  const float amps = (usbC ? app.snap.ic_ma : app.snap.ia_ma) / 1000.0f;
  const float watts = usbC ? app.snap.power_c_w : app.snap.power_a_w;
  const PortStats& port = usbC ? app.session.portC : app.session.portA;

  gfxText(frame, 4, 14, usbC ? "USB-C" : "USB-A", accent, COL_BLACK, 1);
  gfxText(frame, w / 2, 14, SW3518::protocolName(app.snap.protocol), COL_LIGHTGREY, COL_BLACK, 1,
          true);

  char buf[32];
  snprintf(buf, sizeof(buf), "%.2fW", watts);
  gfxText(frame, w / 2, 28, buf, COL_WHITE, COL_BLACK, 2, true);

  snprintf(buf, sizeof(buf), "%.2fV", app.snap.vout_mv / 1000.0f);
  gfxText(frame, 4, 52, buf, COL_LIGHTGREY, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA pk%.2f", amps, port.peakA);
  gfxText(frame, w / 2 - 10, 52, buf, COL_WHITE, COL_BLACK, 1);

  char span[24], label[36];
  formatDuration(app.session.hasData() ? app.session.chargedMs : histSpanMs(app.session), span,
                 sizeof(span));
  snprintf(label, sizeof(label), "span %s", span);
  gfxText(frame, 4, 66, label, COL_LIGHTGREY, COL_BLACK, 1);
}

static void drawHistoryPage() {
  auto& frame = app.frame;
  frame.fillScreen(COL_BLACK);
  const bool showSaved = (app.histFace == HistFace::Saved && app.savedOk);
  const Session& view = showSaved ? app.saved : app.session;
  drawStatusBar(false);
  gfxText(frame, 4, 14, showSaved ? "SAVED" : "SESSION", showSaved ? COL_ORANGE : COL_CYAN,
          COL_BLACK, 1);

  char buf[40], dur[16], ip[20];
  if (view.hasData()) formatDuration(view.chargedMs, dur, sizeof(dur));
  else snprintf(dur, sizeof(dur), "--");
  netIpText(ip, sizeof(ip));
  gfxText(frame, 4, 26, dur, COL_WHITE, COL_BLACK, 1);
  gfxText(frame, frame.width() - 4, 26, ip, COL_CYAN, COL_BLACK, 1, false, true);

  const float wh = view.mwh / 1000.0f;
  snprintf(buf, sizeof(buf), "%.1fW", view.peakW);
  gfxText(frame, 4, 38, "PEAK", COL_LIGHTGREY, COL_BLACK, 1);
  gfxText(frame, 4, 48, buf, COL_WHITE, COL_BLACK, 2);

  snprintf(buf, sizeof(buf), "%.1fW", view.avgW());
  gfxText(frame, 88, 38, "AVG", COL_LIGHTGREY, COL_BLACK, 1);
  gfxText(frame, 88, 48, buf, COL_WHITE, COL_BLACK, 2);

  snprintf(buf, sizeof(buf), "%.3fWh", wh);
  gfxText(frame, 168, 38, "ENERGY", COL_LIGHTGREY, COL_BLACK, 1);
  gfxText(frame, 168, 48, buf, COL_WHITE, COL_BLACK, 1);

  snprintf(buf, sizeof(buf), "C pk %.1fW @%.2fA", view.portC.peakW, view.portC.ampsAtPeakW);
  gfxText(frame, 4, 68, buf, COL_YELLOW, COL_BLACK, 1);
  drawSparkline(frame, 4, 78, 110, 20, view.histC, COL_YELLOW, view.histCount);

  snprintf(buf, sizeof(buf), "A pk %.1fW @%.2fA", view.portA.peakW, view.portA.ampsAtPeakW);
  gfxText(frame, 122, 68, buf, COL_MAGENTA, COL_BLACK, 1);
  drawSparkline(frame, 122, 78, 110, 20, view.histA, COL_MAGENTA, view.histCount);

  snprintf(buf, sizeof(buf), "Vout pk %.2fV", view.peakVoutMv / 1000.0f);
  gfxText(frame, 4, 102, buf, COL_LIGHTGREY, COL_BLACK, 1);
  gfxText(frame, 4, 116, showSaved ? "SAVED 15s then back" : "long=clear  x3=mode", COL_LIGHTGREY,
          COL_BLACK, 1);
}

static void drawModeToast() {
  if (!app.modeToastUntil || millis() > app.modeToastUntil) return;
  const int tw = (int)strlen(app.modeToast) * 12 + 16;
  const int x = (app.frame.width() - tw) / 2;
  app.frame.fillRoundRect(x, 48, tw, 28, 4, COL_CYAN);
  gfxText(app.frame, app.frame.width() / 2, 56, app.modeToast, COL_BLACK, COL_CYAN, 2, true);
}

static void drawHidFrame() {
  app.frame.fillScreen(COL_BLACK);
#if HAS_HID
  switch (app.hidPage) {
    case HidTools::Page::Status:
      HidTools::drawStatus(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
      break;
    case HidTools::Page::Keys:
      HidTools::drawKeys(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
      break;
    case HidTools::Page::Mouse:
      HidTools::drawMouse(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
      break;
    case HidTools::Page::Macros:
      HidTools::drawMacros(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
      break;
    default:
      HidTools::drawHelp(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
      break;
  }
#endif
  drawStatusBar(false);
  drawModeToast();
  uiPush();
}

static void drawRadioFrame() {
  app.frame.fillScreen(COL_BLACK);
#if HAS_RADIO
  const bool wifiUp = netWifiUp();
  const int8_t rssi = netRssi();
  switch (app.radioPage) {
    case RadioPage::WifiScan:
      RadioTools::drawApList(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
      break;
    case RadioPage::Waterfall:
      RadioTools::drawWaterfall(app.frame, COL_WHITE, COL_LIGHTGREY, COL_MAGENTA, COL_CYAN,
                                COL_BLACK);
      break;
    case RadioPage::BleScan:
      RadioTools::drawBleList(app.frame, COL_WHITE, COL_LIGHTGREY, COL_GREEN, COL_BLACK);
      break;
    case RadioPage::Sys:
      RadioTools::drawSys(app.frame, COL_WHITE, COL_LIGHTGREY, COL_CYAN, COL_BLACK, wifiUp, rssi,
                          netMqttOk(), netWebStarted(), app.lastLoopUs, app.loopsPerSec);
      break;
    case RadioPage::Help:
    default:
      RadioTools::drawHelp(app.frame, COL_WHITE, COL_LIGHTGREY, COL_CYAN, COL_BLACK);
      break;
  }
#endif
  drawStatusBar(false);
  drawModeToast();
  uiPush();
}

void uiDraw(uint32_t now) {
  if (app.mode == Mode::Radio) {
    drawRadioFrame();
    return;
  }
  if (app.mode == Mode::Hid) {
    drawHidFrame();
    return;
  }

  uiFinishAnim(now);
  app.frame.fillScreen(COL_BLACK);

  if (app.anim.busy()) {
    const float t = Anim::ease(app.anim.rawT(now));
    const Page detail = (app.anim.kind == Anim::ZoomIn) ? app.anim.to : app.anim.from;
    const bool usbC = (detail == Page::UsbC);
    const float* data = usbC ? app.session.histC : app.session.histA;
    const uint16_t color = usbC ? COL_YELLOW : COL_MAGENTA;
    Rect mini = usbC ? sparkMiniC() : sparkMiniA();
    Rect full = sparkPort();

    if (detail == Page::History) {
      if (app.anim.kind == Anim::ZoomIn) {
        drawMainChrome();
        Rect mc = sparkMiniC(), ma = sparkMiniA();
        drawSparkline(app.frame, (int)mc.x, (int)mc.y, (int)mc.w, (int)mc.h, app.session.histC,
                      COL_YELLOW, app.session.histCount);
        drawSparkline(app.frame, (int)ma.x, (int)ma.y, (int)ma.w, (int)ma.h, app.session.histA,
                      COL_MAGENTA, app.session.histCount);
        if (t > 0.45f) {
          app.frame.fillScreen(COL_BLACK);
          drawHistoryPage();
        }
      } else {
        drawHistoryPage();
        if (t > 0.55f) {
          app.frame.fillScreen(COL_BLACK);
          drawMainChrome();
          Rect mc = sparkMiniC(), ma = sparkMiniA();
          drawSparkline(app.frame, (int)mc.x, (int)mc.y, (int)mc.w, (int)mc.h, app.session.histC,
                        COL_YELLOW, app.session.histCount);
          drawSparkline(app.frame, (int)ma.x, (int)ma.y, (int)ma.w, (int)ma.h, app.session.histA,
                        COL_MAGENTA, app.session.histCount);
        }
      }
      drawModeToast();
      uiPush();
      return;
    }

    float zt = (app.anim.kind == Anim::ZoomIn) ? t : (1.f - t);
    Rect r = lerpRect(mini, full, zt);
    if (zt < 0.55f) {
      drawMainChrome();
      Rect other = usbC ? sparkMiniA() : sparkMiniC();
      const float* otherData = usbC ? app.session.histA : app.session.histC;
      const uint16_t otherCol = usbC ? COL_MAGENTA : COL_YELLOW;
      drawSparkline(app.frame, (int)other.x, (int)other.y, (int)other.w, (int)other.h, otherData,
                    otherCol, app.session.histCount);
    } else {
      drawPortChrome(usbC, zt);
    }
    drawSparkline(app.frame, (int)r.x, (int)r.y, (int)r.w, (int)r.h, data, color,
                  app.session.histCount);
    drawModeToast();
    uiPush();
    return;
  }

  if (app.page == Page::Main) {
    drawMainChrome();
    Rect mc = sparkMiniC(), ma = sparkMiniA();
    drawSparkline(app.frame, (int)mc.x, (int)mc.y, (int)mc.w, (int)mc.h, app.session.histC,
                  COL_YELLOW, app.session.histCount);
    drawSparkline(app.frame, (int)ma.x, (int)ma.y, (int)ma.w, (int)ma.h, app.session.histA,
                  COL_MAGENTA, app.session.histCount);
  } else if (app.page == Page::History) {
    drawHistoryPage();
  } else {
    const bool usbC = (app.page == Page::UsbC);
    drawPortChrome(usbC, 1.f);
    Rect r = sparkPort();
    drawSparkline(app.frame, (int)r.x, (int)r.y, (int)r.w, (int)r.h,
                  usbC ? app.session.histC : app.session.histA, usbC ? COL_YELLOW : COL_MAGENTA,
                  app.session.histCount);
  }
  drawModeToast();
  uiPush();
}
