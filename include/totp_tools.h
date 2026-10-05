#pragma once
#include "features.h"
#if HAS_TOTP

#include <Arduino.h>
#include <Adafruit_GFX.h>

class TotpTools {
public:
  static void begin();
  static void tick(uint32_t now);
  static void draw(GFXcanvas16& canvas);
};

#endif
