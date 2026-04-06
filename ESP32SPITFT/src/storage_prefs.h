#pragma once

#include <Arduino.h>

void initStoragePrefs();

String getStageKey(int stage);
String getStageTitle(int stage);

void loadSavedConfig();
void saveCurrentStageValue();
String loadStageValue(int stage);