#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

void initI2SOut();
void resetPlaybackBuffer();
void pushUdpAudioToPlayback(const uint8_t* data, size_t lenBytes);
size_t playbackAvailableSamples();
void taskAudioPlayback(void* pvParameters);