#pragma once

#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <stdint.h>

void initDisplayHardware();
void initTouchHardware();

TFT_eSPI& display();
XPT2046_Bitbang& touch();

void screenLock();
void screenUnlock();

bool getTouchPoint(uint16_t &x, uint16_t &y);
void waitTouchRelease();