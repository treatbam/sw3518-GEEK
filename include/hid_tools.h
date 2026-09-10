#pragma once

#include <Adafruit_GFX.h>
#include <stdint.h>
#include "features.h"

// Keymod-inspired HID mode: USB (+CDC composite) and BLE keyboard/mouse.
namespace HidTools {

enum class Page : uint8_t { Status = 0, Keys = 1, Mouse = 2, Macros = 3, Help = 4, Count = 5 };

void begin();
void enter();
void leave();
void tick(uint32_t now);

bool usbReady();
bool bleConnected();
bool bleAdvertising();

void keyChar(char c);
void mouseMove(int8_t dx, int8_t dy);
void mouseClick(uint8_t button = 1);
void mouseWheel(int8_t delta);
void runMacro(uint8_t id);
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
