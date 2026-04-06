#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

extern volatile bool commitRequested;
extern volatile bool cancelRequested;

extern QueueHandle_t udpAudioQueue;
extern QueueHandle_t udpControlQueue;

// ====================== APP SCREEN ======================
enum AppScreen {
  SCREEN_CONFIG,
  SCREEN_MAIN
};

// ====================== NETWORK STATUS ======================
enum Status {
  DISCONNECTED,
  WIFI_CONNECTED,
  UDP_CONNECTED
};

// ====================== UDP QUEUE PACKET ======================
struct UdpPacket_t {
  uint8_t* data;
  size_t length;
  bool isAudio;
};

// ====================== AUDIO HEADER ======================
struct AudioHeader {
  uint32_t sequence;
} __attribute__((packed));

// ====================== SHARED STATE ======================
extern volatile AppScreen currentScreen;

extern volatile Status wifiStatus;
extern volatile Status udpStatus;
extern volatile bool micStreaming;

// ====================== CONFIG VALUES ======================
extern String wifiSSID;
extern String wifiPASS;
extern String serverIP;

// ====================== NETWORK STATE ======================
extern unsigned long lastPingTime;
extern unsigned long lastPongTime;
extern bool udpSocketStarted;

// ====================== SHARED RTOS OBJECTS ======================
extern SemaphoreHandle_t screenMutex;

// ====================== CONFIG SCREEN STATE ======================
extern String inputText;
extern bool capsLock;
extern bool symbolMode;
extern int CURRENT_STAGE;
extern String currentTitle;

// ====================== TOUCH ======================
extern uint16_t t_x;
extern uint16_t t_y;

// ====================== AUDIO ======================
extern uint32_t sequence;