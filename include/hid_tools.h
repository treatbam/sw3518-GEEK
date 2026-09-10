#pragma once

#include <Adafruit_GFX.h>
#include <stdint.h>

// Keymod-inspired HID mode: USB (+CDC composite) and BLE keyboard/mouse.
namespace HidTools {

enum class Page : uint8_t { Status = 0, Keys = 1, Mouse = 2, Macros = 3, Help = 4, Count = 5 };

void begin();   // call once from setup (USB HID attaches beside CDC)
void enter();   // start BLE advertise; leave Radio BLE scan first
void leave();   // stop BLE advertise
void tick(uint32_t now);

bool usbReady();
bool bleConnected();
bool bleAdvertising();

// Actions — route to both USB (if ready) and BLE (if connected)
void keyTap(uint8_t keycode, uint8_t modifier = 0);
void keyChar(char c);
void mouseMove(int8_t dx, int8_t dy);
void mouseClick(uint8_t button = 1);  // 1=left 2=right
void mouseWheel(int8_t delta);
void runMacro(uint8_t id);  // 0..n-1
void actionEnter();
void actionEsc();
void actionTab();
void actionArrowLeft();
void actionArrowRight();

const char* pageName(Page p);
const char* macroName(uint8_t id);
uint8_t macroCount();

void drawStatus(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);
void drawKeys(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);
void drawMouse(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);
void drawMacros(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);
void drawHelp(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent, uint16_t bg);

}  // namespace HidTools
