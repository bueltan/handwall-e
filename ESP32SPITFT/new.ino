#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <driver/i2s.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <ctype.h>

// ====================== PINOUT ======================
#define I2S_WS 17
#define I2S_SCK 4
#define I2S_SD 16
#define I2S_PORT I2S_NUM_0

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ====================== OBJECTS ======================
XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button btnWifi, btnSend, btnMic, btnCommit, btnCancel;
WiFiUDP udp;
Preferences prefs;

// ====================== CONFIG KEYBOARD BUTTONS ======================
const int MAX_BUTTONS = 50;
TFT_eSPI_Button keyBtn[MAX_BUTTONS];
String keyLabels[MAX_BUTTONS];
const unsigned long DEL_HOLD_MS = 1000;

// ====================== APP SCREEN ======================
enum AppScreen {
  SCREEN_CONFIG,
  SCREEN_MAIN
};

volatile AppScreen currentScreen = SCREEN_CONFIG;

// ====================== STATES ======================
enum Status { DISCONNECTED, WIFI_CONNECTED, UDP_CONNECTED };
volatile Status wifiStatus = DISCONNECTED;
volatile Status udpStatus  = DISCONNECTED;
volatile bool micStreaming = false;

// ====================== CONFIG VALUES ======================
String wifiSSID = "";
String wifiPASS = "";
String serverIP = "";

const int udpPort = 5000;
const int font_size_s = 1;

// ====================== AUDIO ======================
#define SAMPLE_RATE 16000
#define SAMPLES_PER_PACKET 160
#define BUFFER_LEN SAMPLES_PER_PACKET
#define PACKET_SIZE (BUFFER_LEN * 2)

// ====================== AUDIO HEADER ======================
typedef struct __attribute__((packed)) {
  uint32_t sequence;
} AudioHeader;

// ====================== UDP QUEUE ======================
QueueHandle_t udpSendQueue = NULL;

typedef struct {
  uint8_t* data;
  size_t length;
  bool isAudio;
} UdpPacket_t;

// ====================== GLOBAL BUFFERS ======================
uint32_t sequence = 0;
uint8_t packet[sizeof(AudioHeader) + PACKET_SIZE];
int32_t i2sBuffer[BUFFER_LEN];
uint8_t udpPacket[PACKET_SIZE];

// ====================== NETWORK TIMERS ======================
unsigned long lastPingTime = 0;
unsigned long lastPongTime = 0;
const unsigned long pingInterval = 2000;
const unsigned long pongTimeout  = 3000;

// ====================== SYNC ======================
SemaphoreHandle_t screenMutex = NULL;

// ====================== TOUCH CALIBRATION ======================
int TOUCH_MIN_X = 25;
int TOUCH_MAX_X = 222;
int TOUCH_MIN_Y = 31;
int TOUCH_MAX_Y = 281;

bool swapXY  = true;
bool invertX = true;
bool invertY = false;

// ====================== KEYBOARD STATE ======================
String inputText = "";
const uint8_t maxLength = 150;
uint16_t t_x = 0, t_y = 0;
bool capsLock = false;
bool symbolMode = false;

int CURRENT_STAGE = 0;   // 0: WiFi Name, 1: WiFi Password, 2: Server IP
String currentTitle = "WiFi Name:";

// ====================== TEXT MODE KEYS ======================
const char *row1Text[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
const char *row2Text[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
const char *row3Text[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "."};
const char *row4Text[] = {"Z", "X", "C", "V", "B", "N", "M"};

// ====================== SYMBOL MODE KEYS ======================
const char *row1Sym[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
const char *row2Sym[] = {"@", "#", "$", "%", "&", "/", "\\", ":", "_", "-"};
const char *row3Sym[] = {".", ",", ";", "!", "?", "\"", "'", "(", ")", "="};
const char *row4Sym[] = {"+", "*", "[", "]", "{", "}", "<", ">"};

// ====================== COUNTS ======================
const int row1TextCount = sizeof(row1Text) / sizeof(row1Text[0]);
const int row2TextCount = sizeof(row2Text) / sizeof(row2Text[0]);
const int row3TextCount = sizeof(row3Text) / sizeof(row3Text[0]);
const int row4TextCount = sizeof(row4Text) / sizeof(row4Text[0]);

const int row1SymCount = sizeof(row1Sym) / sizeof(row1Sym[0]);
const int row2SymCount = sizeof(row2Sym) / sizeof(row2Sym[0]);
const int row3SymCount = sizeof(row3Sym) / sizeof(row3Sym[0]);
const int row4SymCount = sizeof(row4Sym) / sizeof(row4Sym[0]);

// ====================== SPECIAL BUTTON INDEXES ======================
const int CAPS_INDEX = 40;
const int SPACE_INDEX = 41;
const int DEL_INDEX = 42;
const int OK_INDEX = 43;
const int MODE_INDEX = 44; // FN / ABC

// ====================== COLORS ======================
const uint16_t UI_BG_COLOR      = TFT_BLACK;
const uint16_t UI_BORDER_COLOR  = TFT_GREEN;
const uint16_t UI_TEXT_COLOR    = TFT_GREEN;
const uint16_t KEY_FILL_COLOR   = TFT_BLACK;
const uint16_t KEY_TEXT_COLOR   = TFT_GREEN;
const uint16_t KEY_BORDER_COLOR = TFT_GREEN;

// ====================== FORWARD DECLARATIONS ======================
void drawMainUI();
void drawStatus(const char* msg);
void drawLog(const char* msg);
void drawVolumeBar(int16_t volume);
void connectWiFi();
void sendUDP(String msg);
void waitTouchRelease();
void advanceToMainScreen();

// ====================== SAFE SCREEN LOCK HELPERS ======================
inline void screenLock() {
  if (screenMutex != NULL) xSemaphoreTake(screenMutex, portMAX_DELAY);
}

inline void screenUnlock() {
  if (screenMutex != NULL) xSemaphoreGive(screenMutex);
}

// ====================== TOUCH RAW -> SCREEN ======================
bool getTouchPoint(uint16_t &x, uint16_t &y) {
  const int samples = 8;
  long sumX = 0;
  long sumY = 0;
  int valid = 0;

  for (int i = 0; i < samples; i++) {
    TouchPoint p = ts.getTouch();

    if (!(p.x == 0 && p.y == 0)) {
      sumX += p.x;
      sumY += p.y;
      valid++;
    }
    delay(2);
  }

  if (valid < 3) return false;

  int rawX = sumX / valid;
  int rawY = sumY / valid;

  if (swapXY) {
    int tmp = rawX;
    rawX = rawY;
    rawY = tmp;
  }

  int sx = map(rawX, TOUCH_MIN_X, TOUCH_MAX_X, 0, 239);
  int sy = map(rawY, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, 319);

  if (invertX) sx = 239 - sx;
  if (invertY) sy = 319 - sy;

  sx = constrain(sx, 0, 239);
  sy = constrain(sy, 0, 319);

  x = sx;
  y = sy;
  return true;
}

void waitTouchRelease() {
  uint16_t rx, ry;
  unsigned long start = millis();

  while (millis() - start < 250) {
    if (!getTouchPoint(rx, ry)) break;
    delay(8);
  }
}

// ====================== PREFERENCES HELPERS ======================
String getStageKey(int stage) {
  if (stage == 0) return "wifi_ssid";
  if (stage == 1) return "wifi_pass";
  return "server_ip";
}

String getStageTitle(int stage) {
  if (stage == 0) return "WiFi Name:";
  if (stage == 1) return "WiFi Password:";
  return "Server IP:";
}

bool shouldShowConnectButton() {
  return (CURRENT_STAGE == 2 && inputText.length() > 0);
}

void loadSavedConfig() {
  if (prefs.getString("wifi_ssid", "") == "") {
    prefs.putString("wifi_ssid", "Galaxy M12 C9A6");
  }
  if (prefs.getString("wifi_pass", "") == "") {
    prefs.putString("wifi_pass", "zhof1469");
  }
  if (prefs.getString("server_ip", "") == "") {
    prefs.putString("server_ip", "10.19.187.13");
  }

  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPASS = prefs.getString("wifi_pass", "");
  serverIP = prefs.getString("server_ip", "");
}

void saveCurrentStageValue() {
  prefs.putString(getStageKey(CURRENT_STAGE).c_str(), inputText);
  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPASS = prefs.getString("wifi_pass", "");
  serverIP = prefs.getString("server_ip", "");
}

void loadStageIntoInput() {
  currentTitle = getStageTitle(CURRENT_STAGE);
  inputText = prefs.getString(getStageKey(CURRENT_STAGE).c_str(), "");
  symbolMode = false;

  drawConfigTitle(currentTitle);
  drawConfigInputBox();
  drawStageProgress();
  redrawKeyRows();
  drawBottomRow();
}

// ====================== CONFIG UI ======================
void drawConfigTitle(const String &title) {
  const int x = 2;
  const int y = 0;
  const int w = 236;
  const int h = 18;

  tft.fillRect(x, y, w, h, UI_BG_COLOR);
  tft.drawRect(x, y, w, h, UI_BORDER_COLOR);

  tft.setTextColor(UI_TEXT_COLOR, UI_BG_COLOR);
  tft.setTextSize(1);
  tft.setCursor(x + 6, y + 5);
  tft.print(title);
}

void drawConfigInputBox() {
  const int boxX = 2;
  const int boxY = 20;
  const int boxW = 236;
  const int boxH = 44;

  tft.fillRect(boxX, boxY, boxW, boxH, UI_BG_COLOR);
  tft.drawRect(boxX, boxY, boxW, boxH, UI_BORDER_COLOR);

  tft.setTextColor(UI_TEXT_COLOR, UI_BG_COLOR);
  tft.setTextSize(1);

  String visibleText = inputText;
  const int maxCharsPerLine = 28;
  const int maxVisible = maxCharsPerLine * 2;

  if (visibleText.length() > maxVisible) {
    visibleText = visibleText.substring(visibleText.length() - maxVisible);
  }

  String line1 = "";
  String line2 = "";

  if (visibleText.length() <= maxCharsPerLine) {
    line1 = visibleText;
  } else {
    line1 = visibleText.substring(0, maxCharsPerLine);
    line2 = visibleText.substring(maxCharsPerLine);
  }

  tft.setCursor(boxX + 8, boxY + 8);
  tft.print(line1);

  tft.setCursor(boxX + 8, boxY + 24);
  tft.print(line2);

  if (inputText.length() < maxLength) {
    tft.print("|");
  }
}

void drawStageProgress() {
  const int totalStages = 3;
  const int x = 2;
  const int y = 82;
  const int h = 3;
  const int gap = 4;
  const int totalW = 236;
  const int barW = (totalW - (gap * (totalStages - 1))) / totalStages;

  tft.fillRect(0, y - 1, 240, h + 2, UI_BG_COLOR);

  for (int i = 0; i < totalStages; i++) {
    int bx = x + i * (barW + gap);

    if (i <= CURRENT_STAGE) {
      tft.fillRect(bx, y, barW, h, UI_BORDER_COLOR);
    } else {
      tft.drawRect(bx, y, barW, h, UI_BORDER_COLOR);
    }
  }
}

String formatKey(const char *key) {
  String s = String(key);

  if (!symbolMode && s.length() == 1 && isalpha((unsigned char)s[0])) {
    char c = s[0];
    c = capsLock ? toupper(c) : tolower(c);
    return String(c);
  }

  return s;
}

void clearKeyLabels() {
  for (int i = 0; i < MAX_BUTTONS; i++) {
    keyLabels[i] = "";
  }
}

void drawButtonAtIndex(int index, int x, int y, int w, int h,
                       const String &label,
                       uint16_t outline, uint16_t fill, uint16_t text,
                       bool inverted = false) {
  if (index < 0 || index >= MAX_BUTTONS) return;

  keyBtn[index].initButton(&tft, x + w / 2, y + h / 2, w, h,
                           outline, fill, text,
                           (char *)label.c_str(), 1);
  keyBtn[index].drawButton(inverted);
  keyLabels[index] = label;
}

int drawRow(int startIndex, int startX, int y,
            const char *keys[], int keyCount,
            int w, int h, int spacing) {
  int x = startX;
  int i = startIndex;

  for (int k = 0; k < keyCount; k++) {
    if (i >= MAX_BUTTONS) break;

    String label = formatKey(keys[k]);
    drawButtonAtIndex(i, x, y, w, h, label,
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);
    x += w + spacing;
    i++;
  }

  return i;
}

void getActiveRows(const char ***r1, int &c1,
                   const char ***r2, int &c2,
                   const char ***r3, int &c3,
                   const char ***r4, int &c4) {
  if (!symbolMode) {
    *r1 = row1Text; c1 = row1TextCount;
    *r2 = row2Text; c2 = row2TextCount;
    *r3 = row3Text; c3 = row3TextCount;
    *r4 = row4Text; c4 = row4TextCount;
  } else {
    *r1 = row1Sym; c1 = row1SymCount;
    *r2 = row2Sym; c2 = row2SymCount;
    *r3 = row3Sym; c3 = row3SymCount;
    *r4 = row4Sym; c4 = row4SymCount;
  }
}

void redrawKeyRows() {
  tft.fillRect(0, 85, 240, 170, UI_BG_COLOR);

  const char **r1;
  const char **r2;
  const char **r3;
  const char **r4;
  int c1, c2, c3, c4;

  getActiveRows(&r1, c1, &r2, c2, &r3, c3, &r4, c4);

  int index = 0;
  index = drawRow(index, 2, 90,  r1, c1, 20, 30, 4);
  index = drawRow(index, 2, 130, r2, c2, 20, 30, 4);
  index = drawRow(index, 2, 170, r3, c3, 20, 30, 4);
  drawRow(index, 2, 210, r4, c4, 20, 30, 4);
}

void drawBottomRow() {
  int y = 255;
  int h = 35;

  tft.fillRect(0, y, 240, h + 2, UI_BG_COLOR);

  drawButtonAtIndex(CAPS_INDEX, 2, y, 42, h, "CAPS",
                    KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

  drawButtonAtIndex(SPACE_INDEX, 48, y, 68, h, "SPACE",
                    KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

  drawButtonAtIndex(DEL_INDEX, 120, y, 34, h, "DEL",
                    KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

  keyLabels[SPACE_INDEX] = "SPACE";

  if (shouldShowConnectButton()) {
    drawButtonAtIndex(OK_INDEX, 158, y, 80, h, "CONNECT",
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

    keyLabels[OK_INDEX] = "CONNECT";
    keyLabels[MODE_INDEX] = "";
  } else {
    drawButtonAtIndex(OK_INDEX, 158, y, 34, h, "OK",
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

    String modeLabel = symbolMode ? "ABC" : "FN";
    drawButtonAtIndex(MODE_INDEX, 196, y, 42, h, modeLabel,
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

    keyLabels[OK_INDEX] = "ENTER";
  }
}

void drawKeyboard() {
  clearKeyLabels();
  redrawKeyRows();
  drawBottomRow();
}

void drawConfigScreenNoMutex() {
  tft.fillScreen(UI_BG_COLOR);
  drawConfigTitle(currentTitle);
  drawConfigInputBox();
  drawStageProgress();
  drawKeyboard();
}

void drawConfigScreen() {
  screenLock();
  tft.fillScreen(UI_BG_COLOR);
  drawConfigTitle(currentTitle);
  drawConfigInputBox();
  drawStageProgress();
  drawKeyboard();
  screenUnlock();
}

void advanceToMainScreen() {
  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPASS = prefs.getString("wifi_pass", "");
  serverIP = prefs.getString("server_ip", "");

  currentScreen = SCREEN_MAIN;
  drawMainUI();
  drawStatus("Ready");
}

// ====================== MAIN UI ======================
void drawMainUI() {
  screenLock();
  tft.fillScreen(TFT_BLACK);

  btnWifi.initButton(&tft, 60, 35, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"WiFi", font_size_s);
  btnWifi.drawButton(false);

  btnSend.initButton(&tft, 180, 35, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"SEND", font_size_s);
  btnSend.drawButton(false);

  btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"MIC OFF", font_size_s);
  btnMic.drawButton(false);

  btnCancel.initButton(&tft, 120, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"CANCEL", font_size_s);
  btnCancel.drawButton(false);

  btnCommit.initButton(&tft, 195, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"COMMIT", font_size_s);
  btnCommit.drawButton(false);

  tft.drawRect(10, 70, 220, 80, TFT_GREEN);
  tft.setCursor(20, 80);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(font_size_s);
  tft.println("Audio Level");

  tft.setCursor(20, 170);
  tft.println("Logs:");

  screenUnlock();
}

void drawStatus(const char* msg) {
  screenLock();
  tft.fillRect(15, 200, 220, 55, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(20, 210);
  tft.println(msg);

  tft.setCursor(20, 242);
  tft.printf("WiFi:%s UDP:%s Mic:%s",
             (wifiStatus == WIFI_CONNECTED) ? "OK" : "NO",
             (udpStatus  == UDP_CONNECTED)  ? "OK" : "NO",
             micStreaming ? "ON" : "OFF");
  screenUnlock();
}

void drawVolumeBar(int16_t volume) {
  screenLock();
  int barWidth = map(constrain(volume, 0, 32767), 0, 32767, 0, 200);
  tft.fillRect(20, 110, 200, 25, TFT_BLACK);
  tft.fillRect(20, 110, barWidth, 25, TFT_GREEN);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(20, 140);
  tft.setTextSize(font_size_s);
  tft.printf("Vol: %5d", volume);
  screenUnlock();
}

void drawLog(const char* msg) {
  screenLock();
  tft.fillRect(20, 215, 200, 18, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(20, 215);
  tft.println(msg);
  screenUnlock();
}

// ====================== I2S ======================
void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 10,
    .dma_buf_len = BUFFER_LEN,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ====================== WIFI / UDP ======================
void connectWiFi() {
  if (wifiSSID.length() == 0) {
    drawStatus("Missing WiFi name");
    return;
  }

  drawStatus("Connecting WiFi...");
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  esp_wifi_set_ps(WIFI_PS_NONE);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiStatus = WIFI_CONNECTED;
    udp.begin(udpPort);
    udpStatus = DISCONNECTED;
    lastPongTime = millis();
    drawStatus("WiFi OK - PS_NONE");
  } else {
    wifiStatus = DISCONNECTED;
    udpStatus = DISCONNECTED;
    drawStatus("WiFi FAILED");
  }
}

void sendUDP(String msg) {
  if (wifiStatus != WIFI_CONNECTED || udpSendQueue == NULL) return;

  size_t len = msg.length();
  uint8_t* buffer = (uint8_t*)malloc(len + 1);
  if (buffer == NULL) return;

  memcpy(buffer, msg.c_str(), len);
  buffer[len] = 0;

  UdpPacket_t pkt = {buffer, len, false};

  if (xQueueSend(udpSendQueue, &pkt, pdMS_TO_TICKS(50)) != pdPASS) {
    free(buffer);
    if (currentScreen == SCREEN_MAIN) drawLog("UDP queue full!");
  }
}

// ====================== UDP SENDER TASK ======================
void taskUDPSender(void *pvParameters) {
  UdpPacket_t pkt;

  while (true) {
    if (xQueueReceive(udpSendQueue, &pkt, portMAX_DELAY) == pdPASS) {
      if (wifiStatus == WIFI_CONNECTED && pkt.data != NULL && serverIP.length() > 0) {
        udp.beginPacket(serverIP.c_str(), udpPort);
        udp.write(pkt.data, pkt.length);
        udp.endPacket();

        if (!pkt.isAudio && currentScreen == SCREEN_MAIN) {
          drawLog("UDP sent");
        }
      }

      if (pkt.data != NULL) {
        free(pkt.data);
        pkt.data = NULL;
      }
    }
  }
}

// ====================== MIC TASK ======================
void taskMicStream(void *pvParameters) {
  static bool i2sReady = false;
  int16_t maxVolume = 0;
  uint32_t lastPrint = 0;

  while (true) {
    if (!i2sReady) {
      initI2S();
      i2sReady = true;
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    if (currentScreen != SCREEN_MAIN || !micStreaming || wifiStatus != WIFI_CONNECTED) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    size_t bytes_read = 0;
    esp_err_t result = i2s_read(I2S_PORT, i2sBuffer, sizeof(i2sBuffer), &bytes_read, portMAX_DELAY);

    if (result != ESP_OK || bytes_read == 0) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    int samples = bytes_read / sizeof(int32_t);
    if (samples != SAMPLES_PER_PACKET) {
      vTaskDelay(2 / portTICK_PERIOD_MS);
      continue;
    }

    maxVolume = 0;

    for (int i = 0; i < samples; i++) {
      int32_t raw = i2sBuffer[i];
      int16_t sample = (int16_t)(raw >> 10);

      if (abs(sample) > maxVolume) maxVolume = abs(sample);

      udpPacket[i * 2]     = (uint8_t)(sample & 0xFF);
      udpPacket[i * 2 + 1] = (uint8_t)(sample >> 8);
    }

    AudioHeader header;
    header.sequence = sequence++;

    memcpy(packet, &header, sizeof(AudioHeader));
    memcpy(packet + sizeof(AudioHeader), udpPacket, samples * 2);

    UdpPacket_t audioPkt;
    audioPkt.data = (uint8_t*)malloc(sizeof(AudioHeader) + samples * 2);

    if (audioPkt.data != NULL) {
      memcpy(audioPkt.data, packet, sizeof(AudioHeader) + samples * 2);
      audioPkt.length = sizeof(AudioHeader) + samples * 2;
      audioPkt.isAudio = true;

      if (xQueueSend(udpSendQueue, &audioPkt, pdMS_TO_TICKS(10)) != pdPASS) {
        free(audioPkt.data);
      }
    }

    if (currentScreen == SCREEN_MAIN) {
      drawVolumeBar(maxVolume);

      if (millis() - lastPrint > 800) {
        lastPrint = millis();
        char logMsg[100];
        snprintf(logMsg, sizeof(logMsg), "Sent seq=%lu smp=%d peak=%d",
                 (unsigned long)header.sequence, samples, maxVolume);
        drawLog(logMsg);
      }
    }

    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

// ====================== CONFIG TOUCH ======================
bool isStillPressingConfigButton(int buttonIndex, unsigned long holdTimeMs) {
  unsigned long start = millis();
  uint16_t x, y;

  while (millis() - start < holdTimeMs) {
    if (!getTouchPoint(x, y)) return false;
    if (!keyBtn[buttonIndex].contains(x, y)) return false;
    delay(10);
  }

  return true;
}

void handleConfigTouch(uint16_t x, uint16_t y) {
  for (int i = 0; i < MAX_BUTTONS; i++) {
    if (keyLabels[i] == "") continue;

    if (keyBtn[i].contains(x, y)) {
      String label = keyLabels[i];

      keyBtn[i].drawButton(true);
      delay(60);
      keyBtn[i].drawButton(false);

      if (label == "CAPS") {
        if (!symbolMode) {
          capsLock = !capsLock;
          redrawKeyRows();
          drawBottomRow();
        }
        waitTouchRelease();
        return;
      }
      else if (label == "FN" || label == "ABC") {
        if (!shouldShowConnectButton()) {
          symbolMode = !symbolMode;
          redrawKeyRows();
          drawBottomRow();
        }
        waitTouchRelease();
        return;
      }
      else if (label == "DEL") {
        bool longPress = isStillPressingConfigButton(i, DEL_HOLD_MS);

        if (longPress) {
          inputText = "";
          drawConfigInputBox();
          drawBottomRow();
          waitTouchRelease();
          return;
        } else {
          if (inputText.length() > 0) {
            inputText.remove(inputText.length() - 1);
            drawConfigInputBox();
            drawBottomRow();
          }
        }
      }
      else if (label == "ENTER") {
        saveCurrentStageValue();

        if (CURRENT_STAGE < 2) {
          CURRENT_STAGE++;
          loadStageIntoInput();
        } else {
          advanceToMainScreen();
        }

        waitTouchRelease();
        return;
      }
      else if (label == "SPACE") {
        if (inputText.length() < maxLength) {
          inputText += " ";
          drawConfigInputBox();
          drawBottomRow();
        }
      }
      else if (label == "CONNECT") {
        saveCurrentStageValue();
        advanceToMainScreen();
        waitTouchRelease();
        return;
      }
      else {
        if (inputText.length() < maxLength) {
          inputText += label;
          drawConfigInputBox();
          drawBottomRow();
        }
      }

      waitTouchRelease();
      return;
    }
  }
}

// ====================== MAIN TOUCH ======================
void handleMainTouch(uint16_t x, uint16_t y) {
  if (btnWifi.contains(x, y)) {
    btnWifi.drawButton(true);
    connectWiFi();
    vTaskDelay(300 / portTICK_PERIOD_MS);
    btnWifi.drawButton(false);
    return;
  }

  if (btnSend.contains(x, y)) {
    btnSend.drawButton(true);
    sendUDP("HELLO_FROM_ESP32 | t=" + String(millis()));
    vTaskDelay(300 / portTICK_PERIOD_MS);
    btnSend.drawButton(false);
    return;
  }

  if (btnMic.contains(x, y)) {
    btnMic.drawButton(true);
    micStreaming = !micStreaming;

    if (micStreaming) {
      btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"MIC ON", font_size_s);
    } else {
      btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"MIC OFF", font_size_s);
    }

    btnMic.drawButton(false);
    drawStatus(micStreaming ? "Audio streaming STARTED" : "Audio streaming STOPPED");
    vTaskDelay(250 / portTICK_PERIOD_MS);
    return;
  }

  if (btnCommit.contains(x, y)) {
    btnCommit.drawButton(true);
    sendUDP("COMMIT");
    vTaskDelay(200 / portTICK_PERIOD_MS);
    btnCommit.drawButton(false);
    drawStatus("Sent COMMIT");
    return;
  }

  if (btnCancel.contains(x, y)) {
    btnCancel.drawButton(true);
    sendUDP("CANCEL");
    vTaskDelay(200 / portTICK_PERIOD_MS);
    btnCancel.drawButton(false);
    drawStatus("Sent CANCEL");
    return;
  }
}

// ====================== TOUCH TASK ======================
void taskTouch(void *pvParameters) {
  while (1) {
    uint16_t x, y;

    if (getTouchPoint(x, y)) {
      if (currentScreen == SCREEN_CONFIG) {
        handleConfigTouch(x, y);
      } else {
        handleMainTouch(x, y);
      }
      waitTouchRelease();
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ====================== UDP TASK ======================
void taskUDP(void *pvParameters) {
  while (1) {
    if (currentScreen == SCREEN_MAIN && wifiStatus == WIFI_CONNECTED) {
      if (millis() - lastPingTime > pingInterval) {
        lastPingTime = millis();
        sendUDP("PING");
      }

      int packetSize = udp.parsePacket();
      if (packetSize) {
        char buffer[64];
        int len = udp.read(buffer, sizeof(buffer) - 1);
        if (len > 0) buffer[len] = 0;

        String msg = String(buffer);

        if (msg == "PONG") {
          lastPongTime = millis();
          udpStatus = UDP_CONNECTED;
          drawStatus("UDP OK");
        } else {
          drawStatus(msg.c_str());
        }
      }

      if (millis() - lastPongTime > pongTimeout) {
        udpStatus = DISCONNECTED;
        drawStatus("UDP LOST");
      }
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ====================== SETUP ======================
void setup() {
  Serial.begin(115200);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  delay(100);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 20);
  tft.println("HANDWALL-E...");
  delay(300);

  ts.begin();
  delay(100);

  prefs.begin("keyboard", false);
  loadSavedConfig();

  CURRENT_STAGE = 0;
  currentTitle = getStageTitle(CURRENT_STAGE);
  inputText = prefs.getString("wifi_ssid", "");

  currentScreen = SCREEN_CONFIG;
  drawConfigScreenNoMutex();
  delay(300);

  screenMutex = xSemaphoreCreateMutex();

  udpSendQueue = xQueueCreate(20, sizeof(UdpPacket_t));
  if (udpSendQueue == NULL) {
    Serial.println("Error creating UDP queue");
  }

  xTaskCreatePinnedToCore(taskTouch,     "TouchTask",  6144, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskMicStream, "MicStream",  8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskUDP,       "UDPTask",    4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskUDPSender, "UDPSender",  6144, NULL, 3, NULL, 1);

  Serial.println("System ready");
}

// ====================== LOOP ======================
void loop() {
  // Everything runs in FreeRTOS tasks
}