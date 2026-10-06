#pragma once
#include <Arduino.h>
#include "features.h"
#include "geek_display.h"

enum class Page : uint8_t { Main = 0, UsbC = 1, UsbA = 2, History = 3 };
enum class Mode : uint8_t { Radio = 1, Totp = 4 };
enum class RadioPage : uint8_t { WifiScan = 0, Waterfall = 1, BleScan = 2, Sys = 3, Help = 4, Count = 5 };
enum class HistFace : uint8_t { Session = 0, Saved = 1 };

static constexpr uint32_t kUiMs = 200;
static constexpr uint32_t kNightIdleMs = 90000;
static constexpr uint32_t kMqttMs = 5000;
static constexpr uint32_t kSdLogMs = 1000;
static constexpr uint32_t kAnimMs = 320;
static constexpr int kBlFull = 255;
static constexpr int kBlDim = 128;
static constexpr uint32_t kWebActiveMs = 8000;
static constexpr int kStatusBarH = 12;

struct Anim {
  enum Kind : uint8_t { Idle = 0, ZoomIn = 1, ZoomOut = 2 } kind = Idle;
  Page from = Page::Main;
  Page to = Page::Main;
  uint32_t startMs = 0;

  float rawT(uint32_t now) const {
    if (kind == Idle) return 1.f;
    float t = (now - startMs) / (float)kAnimMs;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return t;
  }
  static float ease(float t) { return t * t * (3.f - 2.f * t); }
  bool busy() const { return kind != Idle; }
};

struct App {
  GeekDisplay tft;
  GFXcanvas16 frame;

  Mode mode = Mode::Totp;
  Page page = Page::Main;
  RadioPage radioPage = RadioPage::WifiScan;
  uint8_t nextFromMain = 0;
  Anim anim;
  HistFace histFace = HistFace::Session;
  uint32_t histFaceSinceMs = 0;

  bool nightDim = false;
  int blLevel = kBlFull;
  uint32_t lastActivityMs = 0;

  bool wifiEnabled = false;
  bool sdOk = false;
  bool webStarted = false;
  uint32_t lastWebHitMs = 0;
  uint32_t ipShowUntilMs = 0;

  uint32_t lastUiMs = 0;
  uint32_t lastMqttMs = 0;
  uint32_t lastSdMs = 0;
  uint32_t lastBeatMs = 0;

  uint32_t lastLoopUs = 0;
  uint16_t loopsPerSec = 0;

  uint32_t modeToastUntil = 0;
  char modeToast[16] = "";

  HardwareSerial uartDbg;

  App() : frame(240, 135), uartDbg(0) {}
};

extern App app;
