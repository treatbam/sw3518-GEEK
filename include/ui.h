#pragma once

#include <cstdint>
#include "app_state.h"

void uiBeginPanel();
void uiApplyBacklight();
void uiTouchActivity();
void uiShowModeToast(const char* label);
void uiStartZoom(Anim::Kind kind, Page from, Page to);
void uiFinishAnim(uint32_t now);
void uiResetHistFaceTimer(uint32_t now = 0);
void uiTickHistFace(uint32_t now);
void uiDraw(uint32_t now);
void uiPush();
void formatDuration(uint32_t ms, char* out, size_t n);
