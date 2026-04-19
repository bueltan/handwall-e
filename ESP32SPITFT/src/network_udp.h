#pragma once

#include <Arduino.h>

void connectWiFi();
bool enqueueUDPAudio(const uint8_t *data, size_t len);
bool enqueueUDPControl(const String &msg);
void clearUDPAudioQueue();
void clearUDPControlQueue();
void taskUDP(void *pvParameters);
void udpRxTask(void *pvParameters);