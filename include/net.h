#pragma once

#include <cstddef>
#include <cstdint>

void netSetup();
void netTick();
void netPublish();
void netNoteWebHit();

bool netWifiConfigured();
bool netWifiUp();
int8_t netRssi();
int netWifiBars();
void netIpText(char* out, size_t n);
bool netMqttOk();
bool netWebStarted();
bool netWebActive(uint32_t now);
