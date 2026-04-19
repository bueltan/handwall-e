#include "app_state.h"

volatile bool commitRequested = false;
volatile bool cancelRequested = false;

QueueHandle_t udpAudioQueue = NULL;
QueueHandle_t udpControlQueue = NULL;

// ====================== APP SCREEN ======================
volatile AppScreen currentScreen = SCREEN_START;

// ====================== NETWORK STATUS ======================
volatile Status wifiStatus = DISCONNECTED;
volatile Status udpStatus  = DISCONNECTED;
volatile bool micStreaming = false;

// ====================== CONFIG VALUES ======================
String wifiSSID = "";
String wifiPASS = "";
String serverIP = "";

// ====================== NETWORK STATE ======================
unsigned long lastPingTime = 0;
unsigned long lastPongTime = 0;
bool udpSocketStarted = false;

// ====================== SHARED RTOS OBJECTS ======================
SemaphoreHandle_t screenMutex = NULL;

// ====================== CONFIG SCREEN STATE ======================
String inputText = "";
bool capsLock = false;
bool symbolMode = false;
int CURRENT_STAGE = 0;
String currentTitle = "WiFi Name:";

// ====================== TOUCH ======================
uint16_t t_x = 0;
uint16_t t_y = 0;

// ====================== AUDIO ======================
uint32_t sequence = 0;