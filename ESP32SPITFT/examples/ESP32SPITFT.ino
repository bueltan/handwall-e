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
const unsigned long pingInterval = 2000; // enviar ping cada 2s
const unsigned long pongTimeout = 3000;  // si no llega pong en 3s → desconectado

// Último mensaje recibido
String lastMessage = "";

// ---------------------- FUNCIONES DE PANTALLA ----------------------
void drawUI()
{
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
}

void drawStatus() {
  int textHeight = 40; // aprox el alto de la caja de texto (en píxeles)
  int x = 20;
  int y = TFT_HEIGHT - textHeight - 10; // 10 píxeles de margen desde abajo

  tft.fillRect(x, y, TFT_WIDTH - 40, textHeight, TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, y);

  if (wifiStatus == WIFI_CONNECTED) tft.print("WiFi: OK ");
  else tft.print("WiFi: NO ");

  if (udpStatus == UDP_CONNECTED) tft.print(" | UDP: OK");
  else tft.print(" | UDP: NO");
}

void drawMessage(String msg)
{
  int textHeight = 40;
  int x = 20;
  int y = TFT_HEIGHT - textHeight - 10;

  tft.fillRect(x, y, TFT_WIDTH - 40, textHeight, TFT_BLACK);
  tft.setCursor(x, y);
  tft.setTextColor(TFT_YELLOW);
  tft.setTextSize(2);
  tft.println(msg);
}

// ---------------------- FUNCIONES DE RED ----------------------
void connectWiFi() {
  Serial.println("Conectando WiFi...");
  unsigned long start = millis();
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
  }

  unsigned long elapsed = millis() - start;
  Serial.println("WiFi conectado");
  Serial.print("Tiempo conexion: "); Serial.print(elapsed); Serial.println(" ms");

  wifiStatus = WIFI_CONNECTED;
  drawStatus();

  // Inicializamos UDP
  udp.begin(udpPort);
  udpStatus = UDP_CONNECTED;
  drawStatus();
}

void sendUDP() {
  String msg = "HELLO_FROM_ESP32 | t=" + String(millis());
  udp.beginPacket(udpAddress, udpPort);
  udp.print(msg);
  udp.endPacket();

  Serial.println("UDP enviado: " + msg);
}

// ---------------------- REVISAR MENSAJES ----------------------
void checkUDP()
{
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incoming[255];
    int len = udp.read(incoming, 255);
    if (len > 0) incoming[len] = 0;
    lastMessage = String(incoming);

    Serial.println("UDP recibido: " + lastMessage);

    if (lastMessage == "PONG") {
      lastPongTime = millis();
      udpStatus = UDP_CONNECTED;
      drawStatus();
    } else {
      drawMessage(lastMessage);
    }
  }
}

// ---------------------- SETUP ----------------------
void setup()
{
  Serial.begin(115200);
  ts.begin();

  tft.init();
  tft.setRotation(0);

  drawStatus();
  drawUI();
}

// ---------------------- LOOP ----------------------
void loop()
{
  // Leer touch
  TouchPoint p = ts.getTouch();
  int x = constrain(map(p.y, 220, 20, 0, 239), 0, 239);
  int y = constrain(map(p.x, 20, 299, 0, 319), 0, 319);

  if (p.zRaw > 200) {
    if (btnWifi.contains(x,y)) {
      btnWifi.drawButton(true);
      connectWiFi();
      delay(300);
      btnWifi.drawButton(false);
    }

    if (btnSend.contains(x,y)) {
      btnSend.drawButton(true);
      sendUDP();
      delay(300);
      btnSend.drawButton(false);
    }
  }

  // Revisar mensajes UDP entrantes
  checkUDP();

  // Enviar ping periódico
  if (millis() - lastPingTime > pingInterval) {
    lastPingTime = millis();
    udp.beginPacket(udpAddress, udpPort);
    udp.print("PING");
    udp.endPacket();
  }

  // Si no llegó PONG en pongTimeout → desconectado
  if (millis() - lastPongTime > pongTimeout) {
    udpStatus = DISCONNECTED;
    drawStatus();
  }

  delay(20);
}