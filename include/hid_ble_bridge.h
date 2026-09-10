#pragma once
#include <stdint.h>
bool hidBleBegin();
void hidBleEnd();
bool hidBleConnected();
void hidBleWrite(uint8_t k);
void hidBlePress(uint8_t k);
void hidBleReleaseAll();
void hidBleMouseMove(int8_t x, int8_t y, int8_t wheel = 0);
void hidBleMouseClick(uint8_t btn);  // 1=left 2=right
