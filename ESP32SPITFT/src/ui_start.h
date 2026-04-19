#pragma once

#include <Arduino.h>

enum AudioBarMode
{
    AUDIO_BAR_IDLE = 0,
    AUDIO_BAR_MIC,
    AUDIO_BAR_INCOMING
};

void drawStartUI();
void handleStartTouch(uint16_t x, uint16_t y);

void updateStartAudioLevel(int16_t volume, AudioBarMode mode);
void resetStartAudioLevel();

void drawStartConnectionStatus();
void drawStartMicStatus();

void taskStartUI(void *pvParameters);