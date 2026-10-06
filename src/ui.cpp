#include "ui.h"
#include <WiFi.h>
#include "app_state.h"
#include "features.h"
#include "net.h"
#include "radio_tools.h"
#include "totp_tools.h"

#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif


void uiBeginPanel() { app.tft.beginPanel(); }

void uiApplyBacklight() { analogWrite(PIN_TFT_BL, app.blLevel); }

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

void uiPush() { app.tft.push(app.frame); }

static void drawConnIcons(Adafruit_GFX& g, int16_t rightX, int16_t y, bool withIp) {
  if (withIp && app.ipShowUntilMs && millis() < app.ipShowUntilMs && netWifiUp()) {
    const String ip = WiFi.localIP().toString();
    const int16_t w = (int16_t)(ip.length() * 6);
    gfxText(g, rightX - w, y, ip.c_str(), COL_CYAN, COL_BLACK, 1);
    rightX -= (w + 4);
  }

  const uint32_t now = millis();
  const bool webOn = (app.webStarted && now - app.lastWebHitMs < kWebActiveMs);
  const uint16_t webCol = webOn ? COL_WHITE : COL_DIM;

  g.drawRect(rightX - 12, y + 1, 10, 6, webCol);
  g.drawFastHLine(rightX - 9, y + 0, 4, webCol);
  g.drawPixel(rightX - 11, y + 2, webCol);
  g.drawPixel(rightX - 11, y + 4, webCol);

  if (app.wifiEnabled) {
    const bool up = netWifiUp();
    const uint16_t wfCol = up ? COL_CYAN : COL_DIM;
    g.drawFastVLine(rightX - 15, y + 5, 2, wfCol);
    g.drawFastVLine(rightX - 17, y + 3, 4, wfCol);
    g.drawFastVLine(rightX - 19, y + 1, 6, wfCol);

    if (up && MQTT_HOST[0] != 0) {
      const bool mUp = netMqttOk();
      const uint16_t mCol = mUp ? COL_YELLOW : COL_DIM;
      g.drawLine(rightX - 25, y + 3, rightX - 23, y + 1, mCol);
      g.drawLine(rightX - 25, y + 3, rightX - 23, y + 5, mCol);
      g.drawLine(rightX - 21, y + 1, rightX - 23, y + 3, mCol);
      g.drawLine(rightX - 21, y + 5, rightX - 23, y + 3, mCol);
    }
  }
}

static void drawStatusBar(bool withIp) {
  auto& frame = app.frame;
  const int w = frame.width();
  frame.fillRect(0, 0, w, kStatusBarH, COL_BLACK);
  frame.drawFastHLine(0, kStatusBarH - 1, w, COL_DIM);

  const bool radio = (app.mode == Mode::Radio);
  const bool totp = (app.mode == Mode::Totp);
  const char* modeTag = radio ? "RAD" : (totp ? "2FA" : "---");
  const uint16_t modeCol = radio ? COL_MAGENTA : (totp ? COL_CYAN : COL_WHITE);
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
  } else {
    crumbs[n++] = "MAIN";
    active = 0;
  }
  if (active < 0) active = 0;
  if (active >= n) active = n - 1;

  int x = 28;
  for (int i = 0; i < n; i++) {
    const bool on = (i == active);
    gfxText(frame, x, 2, crumbs[i], on ? COL_WHITE : COL_DARKGREY, COL_BLACK, 1);
    const int tw = (int)strlen(crumbs[i]) * 6;
    if (on) {
      frame.drawFastHLine(x, 10, tw, radio ? COL_MAGENTA : COL_CYAN);
    }
    x += tw + 6;
    if (i + 1 < n) gfxText(frame, x - 5, 2, ".", COL_DIM, COL_BLACK, 1);
  }

  drawConnIcons(frame, w - 2, 1, withIp);
}

static void drawModeToast() {
  if (!app.modeToastUntil || millis() > app.modeToastUntil) return;
  const int tw = (int)strlen(app.modeToast) * 12 + 16;
  const int x = (app.frame.width() - tw) / 2;
  app.frame.fillRoundRect(x, 48, tw, 28, 4, COL_CYAN);
  gfxText(app.frame, app.frame.width() / 2, 56, app.modeToast, COL_BLACK, COL_CYAN, 2, true);
}

static void drawRadioFrame(uint32_t now) {
  app.frame.fillScreen(COL_BLACK);
  if (app.radioPage == RadioPage::WifiScan) {
    RadioTools::drawApList(app.frame, COL_WHITE, COL_LIGHTGREY, COL_YELLOW, COL_BLACK);
  } else if (app.radioPage == RadioPage::Waterfall) {
    RadioTools::drawWaterfall(app.frame, COL_WHITE, COL_LIGHTGREY, COL_MAGENTA, COL_CYAN, COL_BLACK);
  } else if (app.radioPage == RadioPage::BleScan) {
    RadioTools::drawBleList(app.frame, COL_WHITE, COL_LIGHTGREY, COL_GREEN, COL_BLACK);
  } else if (app.radioPage == RadioPage::Sys) {
    const bool wifiUp = netWifiUp();
    int rssi = wifiUp ? WiFi.RSSI() : -100;
    const bool webOn = (app.webStarted && now - app.lastWebHitMs < kWebActiveMs);
    RadioTools::drawSys(app.frame, COL_WHITE, COL_LIGHTGREY, COL_CYAN, COL_BLACK, wifiUp, rssi, netMqttOk(), webOn, app.lastLoopUs, app.loopsPerSec);
  } else {
    RadioTools::drawHelp(app.frame, COL_WHITE, COL_LIGHTGREY, COL_CYAN, COL_BLACK);
  }
  drawStatusBar(app.radioPage == RadioPage::Sys);
  drawModeToast();
  uiPush();
}

void uiDraw(uint32_t now) {
  if (app.mode == Mode::Totp) {
    app.frame.fillScreen(COL_BLACK);
    TotpTools::draw(app.frame);
    drawStatusBar(false);
    drawModeToast();
    uiPush();
    return;
  }
  if (app.mode == Mode::Radio) {
    drawRadioFrame(now);
    return;
  }

  app.frame.fillScreen(COL_BLACK);
  drawModeToast();
  uiPush();
}
