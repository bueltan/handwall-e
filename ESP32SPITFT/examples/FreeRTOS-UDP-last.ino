#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>

// Pines del touch
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button btnWifi;
TFT_eSPI_Button btnSend;

WiFiUDP udp;

volatile bool wifiConnectRequested = false;
volatile bool sendHelloRequested = false;

// Estados
enum Status { DISCONNECTED, WIFI_CONNECTED, UDP_CONNECTED };
Status wifiStatus = DISCONNECTED;
Status udpStatus  = DISCONNECTED;

// Configuración WiFi
const char* ssid = "MOVISTAR-WIFI6-B700";
const char* password = "uDRsVe6jpvSfSCiQ9wPL";

// Configuración UDP
const char* udpAddress = "192.168.1.37";
const int udpPort = 5000;

// Variables para ping/pong
unsigned long lastPingTime = 0;
unsigned long lastPongTime = 0;
const unsigned long pingInterval = 2000;
const unsigned long pongTimeout = 3000;

String lastMessage = "";

// Mutex para acceso a pantalla
SemaphoreHandle_t screenMutex;

// ---------------------- FUNCIONES DE PANTALLA ----------------------
void drawUI() {
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  tft.fillScreen(TFT_BLACK);

  btnWifi.initButton(&tft, 80, 40, 140, 50, TFT_WHITE, TFT_BLUE, TFT_WHITE, "WiFi", 2);
  btnWifi.drawButton(false);

  btnSend.initButton(&tft, 200, 40, 140, 50, TFT_WHITE, TFT_GREEN, TFT_WHITE, "SEND", 2);
  btnSend.drawButton(false);

  tft.drawRect(10, 100, 220, 200, TFT_WHITE);

  tft.setCursor(20,110);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.println("UDP RX:");
  xSemaphoreGive(screenMutex);
}

void drawStatus() {
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  int y = 10;
  tft.fillRect(0, y, TFT_WIDTH, 20, TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, y);

  if (wifiStatus == WIFI_CONNECTED) tft.print("WiFi: OK ");
  else tft.print("WiFi: NO ");

  if (udpStatus == UDP_CONNECTED) tft.print(" | UDP: OK");
  else tft.print(" | UDP: NO");
  xSemaphoreGive(screenMutex);
}

void drawMessage(String msg) {
  xSemaphoreTake(screenMutex, portMAX_DELAY);
  int textHeight = 40;
  int x = 20;
  int y = TFT_HEIGHT - textHeight - 10;

  tft.fillRect(x, y, TFT_WIDTH - 40, textHeight, TFT_BLACK);
  tft.setCursor(x, y);
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.println(msg);
  xSemaphoreGive(screenMutex);
}

// ---------------------- RED ----------------------
void connectWiFi() {
  Serial.println("Conectando WiFi...");
  unsigned long start = millis();
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }

  unsigned long elapsed = millis() - start;
  Serial.println("WiFi conectado");
  Serial.print("Tiempo conexion: "); Serial.print(elapsed); Serial.println(" ms");

  wifiStatus = WIFI_CONNECTED;

  udp.begin(udpPort);
  udpStatus = UDP_CONNECTED;
  drawStatus();
}

void sendUDP(String msg) {
  udp.beginPacket(udpAddress, udpPort);
  udp.print(msg);
  udp.endPacket();
  Serial.println("UDP enviado: " + msg);
}

// ---------------------- TAREAS ----------------------

// Tarea de touch y botones
void taskTouch(void *pvParameters) {
  bool touchWasDown = false;

  while (1) {
    TouchPoint p = ts.getTouch();
    int x = constrain(map(p.y, 220, 20, 0, 239), 0, 239);
    int y = constrain(map(p.x, 20, 299, 0, 319), 0, 319);

    bool touchDown = (p.zRaw > 200);

    if (touchDown && !touchWasDown) {
      if (btnWifi.contains(x, y)) {
        btnWifi.drawButton(true);
        wifiConnectRequested = true;
      }
      else if (btnSend.contains(x, y)) {
        btnSend.drawButton(true);
        sendHelloRequested = true;
      }
    }

    if (!touchDown && touchWasDown) {
      btnWifi.drawButton(false);
      btnSend.drawButton(false);
    }

    touchWasDown = touchDown;
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}
// Tarea de UDP
void taskUDP(void *pvParameters) {
  while (1) {
    // solo enviar si WiFi está conectado
    if (wifiStatus == WIFI_CONNECTED) {
      // enviar ping cada pingInterval
      if (millis() - lastPingTime > pingInterval) {
        lastPingTime = millis();
        udp.beginPacket(udpAddress, udpPort);
        udp.print("PING");
        udp.endPacket();
      }

      // revisar mensajes UDP entrantes
      int packetSize = udp.parsePacket();
      if (packetSize) {
        char buffer[64];
        int len = udp.read(buffer, sizeof(buffer)-1);
        if (len > 0) buffer[len] = 0;
        String msg = String(buffer);
        if (msg == "PONG") {
          lastPongTime = millis();
          udpStatus = UDP_CONNECTED;
          drawStatus();
        } else {
          drawMessage(msg);
        }
      }

      // timeout de pong
      if (millis() - lastPongTime > 3000) {
        udpStatus = DISCONNECTED;
        drawStatus();
      }
    }
    
    // espera antes de la siguiente iteración
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ---------------------- SETUP ----------------------
void setup() {
  Serial.begin(115200);
  ts.begin();
  tft.init();
  tft.setRotation(0);
  screenMutex = xSemaphoreCreateMutex();

  drawUI();
  drawStatus();

  
  xTaskCreatePinnedToCore(taskTouch, "TouchTask", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskUDP, "UDPTask", 4096, NULL, 1, NULL, 0);
}

// ---------------------- LOOP ----------------------
void loop() {
  // vacío porque usamos FreeRTOS tasks
}