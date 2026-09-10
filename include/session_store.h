#pragma once

#include <cstdint>

void sessionStoreLoad();
void sessionStoreTick(uint32_t now, bool force = false);
void sessionStoreErase();
void sessionCaptureSaved();
