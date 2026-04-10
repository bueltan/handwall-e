#pragma once
#include <Arduino.h>

void loadStageIntoInput();
void drawConfigTitle(const String &title);
void drawConfigInputBox();
void drawStageProgress();
void redrawKeyRows();
void drawBottomRow();
void drawKeyboard();
void drawConfigScreenNoMutex();
void drawConfigScreen();
void handleConfigTouch(uint16_t x, uint16_t y);
bool isStillPressingConfigButton(int buttonIndex, unsigned long holdTimeMs);
