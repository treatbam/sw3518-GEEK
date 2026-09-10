#include "hid_tools.h"
#include "hid_ble_bridge.h"
#include "features.h"

#include <Arduino.h>
#include <string.h>

#if !HAS_HID

namespace HidTools {
void begin() {}
void enter() {}
void leave() {}
void tick(uint32_t) {}
bool usbReady() { return false; }
bool bleConnected() { return false; }
bool bleAdvertising() { return false; }
void keyChar(char) {}
void mouseMove(int8_t, int8_t) {}
void mouseClick(uint8_t) {}
void mouseWheel(int8_t) {}
void runMacro(uint8_t) {}
void actionEnter() {}
void actionEsc() {}
void actionTab() {}
void actionArrowLeft() {}
void actionArrowRight() {}
const char* pageName(Page) { return "?"; }
const char* macroName(uint8_t) { return ""; }
uint8_t macroCount() { return 0; }
void drawStatus(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawKeys(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawMouse(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawMacros(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
void drawHelp(Adafruit_GFX&, uint16_t, uint16_t, uint16_t, uint16_t) {}
}  // namespace HidTools

#else

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
#define HID_HAS_USB 1
#else
#define HID_HAS_USB 0
#endif

namespace HidTools {
namespace {

#if HID_HAS_USB
USBHIDKeyboard usbKb;
USBHIDMouse usbMs;
bool usbStarted = false;
#endif

bool bleStarted = false;

enum class Action : uint8_t {
  Enter, Esc, Tab, Space, Backspace, Up, Down, Left, Right,
  Copy, Paste, Cut, Undo, SelectAll, Save, Lock,
};

struct Macro {
  const char* name;
  Action action;
};

static const Macro kMacros[] = {
    {"Copy", Action::Copy},
    {"Paste", Action::Paste},
    {"Cut", Action::Cut},
    {"Undo", Action::Undo},
    {"SelectAll", Action::SelectAll},
    {"Save", Action::Save},
    {"Lock", Action::Lock},
};

static void gfxText(Adafruit_GFX& g, int16_t x, int16_t y, const char* s, uint16_t fg, uint16_t bg,
                    uint8_t size = 1, bool centerX = false) {
  g.setTextSize(size);
  g.setTextColor(fg, bg);
  g.setTextWrap(false);
  if (centerX) x = (int16_t)(x - (int)strlen(s) * 6 * size / 2);
  g.setCursor(x, y);
  g.print(s);
}

static void usbChord(uint8_t modKey, uint8_t key) {
#if HID_HAS_USB
  if (!usbStarted) return;
  if (modKey) usbKb.press(modKey);
  usbKb.press(key);
  delay(15);
  usbKb.releaseAll();
#else
  (void)modKey; (void)key;
#endif
}

// BleCombo KEY_* values (Arduino-compatible codes from that lib)
static constexpr uint8_t BKEY_CTRL = 0x80;
static constexpr uint8_t BKEY_GUI = 0x83;
static constexpr uint8_t BKEY_UP = 0xDA;
static constexpr uint8_t BKEY_DOWN = 0xD9;
static constexpr uint8_t BKEY_LEFT = 0xD8;
static constexpr uint8_t BKEY_RIGHT = 0xD7;
static constexpr uint8_t BKEY_BACKSPACE = 0xB2;
static constexpr uint8_t BKEY_TAB = 0xB3;
static constexpr uint8_t BKEY_RETURN = 0xB0;
static constexpr uint8_t BKEY_ESC = 0xB1;

static void bleChord(uint8_t modKey, uint8_t key) {
  if (!bleStarted || !hidBleConnected()) return;
  if (modKey) hidBlePress(modKey);
  hidBlePress(key);
  delay(15);
  hidBleReleaseAll();
}

static void doAction(Action a) {
  switch (a) {
    case Action::Enter:
#if HID_HAS_USB
      if (usbStarted) usbKb.write(KEY_RETURN);
#endif
      if (bleStarted) hidBleWrite(BKEY_RETURN);
      break;
    case Action::Esc:
      usbChord(0, KEY_ESC);
      bleChord(0, BKEY_ESC);
      break;
    case Action::Tab:
      usbChord(0, KEY_TAB);
      bleChord(0, BKEY_TAB);
      break;
    case Action::Space:
      usbChord(0, ' ');
      bleChord(0, ' ');
      break;
    case Action::Backspace:
      usbChord(0, KEY_BACKSPACE);
      bleChord(0, BKEY_BACKSPACE);
      break;
    case Action::Up:
      usbChord(0, KEY_UP_ARROW);
      bleChord(0, BKEY_UP);
      break;
    case Action::Down:
      usbChord(0, KEY_DOWN_ARROW);
      bleChord(0, BKEY_DOWN);
      break;
    case Action::Left:
      usbChord(0, KEY_LEFT_ARROW);
      bleChord(0, BKEY_LEFT);
      break;
    case Action::Right:
      usbChord(0, KEY_RIGHT_ARROW);
      bleChord(0, BKEY_RIGHT);
      break;
    case Action::Copy:
      usbChord(KEY_LEFT_CTRL, 'c');
      bleChord(BKEY_CTRL, 'c');
      break;
    case Action::Paste:
      usbChord(KEY_LEFT_CTRL, 'v');
      bleChord(BKEY_CTRL, 'v');
      break;
    case Action::Cut:
      usbChord(KEY_LEFT_CTRL, 'x');
      bleChord(BKEY_CTRL, 'x');
      break;
    case Action::Undo:
      usbChord(KEY_LEFT_CTRL, 'z');
      bleChord(BKEY_CTRL, 'z');
      break;
    case Action::SelectAll:
      usbChord(KEY_LEFT_CTRL, 'a');
      bleChord(BKEY_CTRL, 'a');
      break;
    case Action::Save:
      usbChord(KEY_LEFT_CTRL, 's');
      bleChord(BKEY_CTRL, 's');
      break;
    case Action::Lock:
      usbChord(KEY_LEFT_GUI, 'l');
      bleChord(BKEY_GUI, 'l');
      break;
  }
}

}  // namespace

void begin() {
#if HID_HAS_USB
  usbKb.begin();
  usbMs.begin();
  usbStarted = true;
  Serial.println("HID: USB keyboard+mouse ready (composite with CDC)");
#endif
}

void enter() {
#if HID_HAS_USB
  if (!usbStarted) {
    usbKb.begin();
    usbMs.begin();
    usbStarted = true;
    Serial.println("HID: USB keyboard+mouse ready (composite with CDC)");
  }
#endif
  if (!bleStarted) bleStarted = hidBleBegin();
  Serial.println(bleStarted ? "HID: BLE Keyboard+Mouse advertising"
                            : "HID: BLE unavailable — USB only");
}

void leave() {
  if (bleStarted) {
    hidBleEnd();
    bleStarted = false;
    Serial.println("HID: BLE stopped");
  }
}

void tick(uint32_t) {}

bool usbReady() {
#if HID_HAS_USB
  return usbStarted;
#else
  return false;
#endif
}

bool bleConnected() { return bleStarted && hidBleConnected(); }

bool bleAdvertising() { return bleStarted && !hidBleConnected(); }

void keyChar(char c) {
#if HID_HAS_USB
  if (usbStarted) usbKb.write((uint8_t)c);
#endif
  if (bleStarted) hidBleWrite((uint8_t)c);
}

void mouseMove(int8_t dx, int8_t dy) {
#if HID_HAS_USB
  if (usbStarted) usbMs.move(dx, dy);
#endif
  if (bleStarted) hidBleMouseMove(dx, dy, 0);
}

void mouseClick(uint8_t button) {
#if HID_HAS_USB
  if (usbStarted) usbMs.click(button == 2 ? MOUSE_RIGHT : MOUSE_LEFT);
#endif
  if (bleStarted) hidBleMouseClick(button == 2 ? 2 : 1);
}

void mouseWheel(int8_t delta) {
#if HID_HAS_USB
  if (usbStarted) usbMs.move(0, 0, delta);
#endif
  if (bleStarted) hidBleMouseMove(0, 0, delta);
}

void runMacro(uint8_t id) {
  if (id >= macroCount()) return;
  doAction(kMacros[id].action);
}

const char* pageName(Page p) {
  switch (p) {
    case Page::Status: return "STAT";
    case Page::Keys: return "KEYS";
    case Page::Mouse: return "MOUSE";
    case Page::Macros: return "MACRO";
    case Page::Help: return "HELP";
    default: return "?";
  }
}

const char* macroName(uint8_t id) {
  if (id >= macroCount()) return "";
  return kMacros[id].name;
}

uint8_t macroCount() { return (uint8_t)(sizeof(kMacros) / sizeof(kMacros[0])); }

void drawStatus(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  gfxText(g, 4, 16, "HID / KeyMod-ish", accent, bg, 1);
  char line[42];
  snprintf(line, sizeof(line), "USB %s", usbReady() ? "HID+CDC ready" : "off");
  gfxText(g, 4, 32, line, fg, bg, 1);
  snprintf(line, sizeof(line), "BLE %s",
           bleConnected() ? "connected" : (bleAdvertising() ? "advertising..." : "off"));
  gfxText(g, 4, 44, line, fg, bg, 1);
  gfxText(g, 4, 64, "USB-A into host PC/TV/SBC", dim, bg, 1);
  gfxText(g, 4, 76, "or pair BLE KeyboardMouse", dim, bg, 1);
  gfxText(g, 4, 100, "Triple BOOT: CHG>RAD>HID", dim, bg, 1);
}

void drawKeys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  gfxText(g, 4, 16, "Keys", accent, bg, 1);
  gfxText(g, 4, 32, "Short: next page", fg, bg, 1);
  gfxText(g, 4, 44, "Long BOOT: Enter", fg, bg, 1);
  gfxText(g, 4, 56, "Double: Esc", fg, bg, 1);
  gfxText(g, 4, 72, "Side L/R: Left/Right", dim, bg, 1);
}

void drawMouse(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  gfxText(g, 4, 16, "Mouse", accent, bg, 1);
  gfxText(g, 4, 32, "Short: next page", fg, bg, 1);
  gfxText(g, 4, 44, "Long BOOT: left click", fg, bg, 1);
  gfxText(g, 4, 56, "Double: right click", fg, bg, 1);
  gfxText(g, 4, 72, "Side L/R: move 12px", dim, bg, 1);
}

void drawMacros(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  gfxText(g, 4, 16, "Macros", accent, bg, 1);
  for (uint8_t i = 0; i < macroCount() && i < 6; i++) {
    char line[28];
    snprintf(line, sizeof(line), "%u %s", (unsigned)(i + 1), macroName(i));
    gfxText(g, 4, 32 + (int)i * 12, line, fg, bg, 1);
  }
  gfxText(g, 4, 112, "Long runs macro; Short next", dim, bg, 1);
}

void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg) {
  gfxText(g, 4, 16, "HID Help", accent, bg, 1);
  gfxText(g, 4, 32, "Openterface KeyMod-style", fg, bg, 1);
  gfxText(g, 4, 48, "Hardware HID, no host app", fg, bg, 1);
  gfxText(g, 4, 64, "Not video KVM (no HDMI)", dim, bg, 1);
  gfxText(g, 4, 80, "CDC shares USB-A — replug", dim, bg, 1);
  gfxText(g, 4, 92, "after flash if port acts up", dim, bg, 1);
  gfxText(g, 4, 112, "Short: next  Dbl: prev page", dim, bg, 1);
}

void actionEnter() { doAction(Action::Enter); }
void actionEsc() { doAction(Action::Esc); }
void actionTab() { doAction(Action::Tab); }
void actionArrowLeft() { doAction(Action::Left); }
void actionArrowRight() { doAction(Action::Right); }

}  // namespace HidTools

#endif  // HAS_HID
