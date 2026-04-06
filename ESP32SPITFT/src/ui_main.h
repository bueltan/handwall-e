#pragma once

#include <stdint.h>

void drawMainUI();
void drawStatus(const char* msg);
void drawLog(const char* msg);
void drawVolumeBar(int16_t volume);
void handleMainTouch(uint16_t x, uint16_t y);