#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>

#include <WiFi.h>
#include <WiFiUdp.h>


const char* ssid = "MOVISTAR-WIFI6-B700";
const char* password = "uDRsVe6jpvSfSCiQ9wPL";

const char* udpAddress = "192.168.1.37";
const int udpPort = 5000;
WiFiUDP udp;


// Pines touch
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button key[6];

int lastPressed = -1;

void setup() {
  Serial.begin(115200);
  delay(100);

  connectWiFi();

  udp.begin(udpPort);

  ts.begin();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  drawButtons();
}

void connectWiFi() {

  WiFi.begin(ssid, password);

  Serial.print("Conectando WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectado");
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());
}

void sendUDP(String msg) {

  udp.beginPacket(udpAddress, udpPort);
  udp.print(msg);
  udp.endPacket();

  Serial.println("UDP enviado: " + msg);
}
void drawButtons() {

  uint16_t bWidth  = TFT_WIDTH  / 3;
  uint16_t bHeight = TFT_HEIGHT / 2;

  for (int i = 0; i < 6; i++) {

    key[i].initButton(
      &tft,
      bWidth * (i % 3) + bWidth / 2,
      bHeight * (i / 3) + bHeight / 2,
      bWidth,
      bHeight,
      TFT_WHITE,
      TFT_BLUE,
      TFT_WHITE,
      "",
      1
    );

    key[i].drawButton(false, String(i + 1));
  }
}

void loop() {

TouchPoint p = ts.getTouch();
/*
 Touch calibration and coordinate mapping

 The XPT2046 touch controller does not return pixel coordinates.
 It returns raw ADC values (typically 0–4095). These raw values must be
 converted to the screen coordinate system.

 Screen resolution:
   TFT display: 240 x 320 pixels (portrait mode)

 Axis relation (due to hardware orientation):
   touch Y -> screen X
   touch X -> screen Y

 Raw touch limits measured during calibration:

   Horizontal movement on screen (X axis):
     left edge  -> p.y ≈ 220
     right edge -> p.y ≈ 20

   Vertical movement on screen (Y axis):
     top edge    -> p.x ≈ 20
     bottom edge -> p.x ≈ 299

 Mapping converts the raw touch range into screen pixel coordinates.

   map(value, in_min, in_max, out_min, out_max)

 X coordinate mapping:
   Converts raw touch Y values (220..20) to screen pixels (0..239)

 Y coordinate mapping:
   Converts raw touch X values (20..299) to screen pixels (0..319)

 constrain() ensures the calculated coordinate stays within
 the valid screen boundaries.

 Example:

   int x = constrain(map(p.y, 220, 20, 0, 239), 0, 239);
   int y = constrain(map(p.x, 20, 299, 0, 319), 0, 319);

 Result:
   Raw touch coordinates -> calibrated screen pixel coordinates
*/
int x = constrain(map(p.y, 220, 20, 0, 239), 0, 239);
int y = constrain(map(p.x, 20, 299, 0, 319), 0, 319);

int current = -1;

if (p.zRaw > 200) {

  Serial.printf("Raw: x=%d y=%d z=%d | Adj: x=%d y=%d\n",
                p.x, p.y, p.zRaw, x, y);

  for (int i = 0; i < 6; i++) {
    if (key[i].contains(x, y)) {
      current = i;
      break;
    }
  }
}

  // Manejo simple de estado de botones
  if (current != lastPressed) {

    if (lastPressed != -1) {
      key[lastPressed].drawButton(false, String(lastPressed + 1));
      Serial.printf("Button %d released\n", lastPressed + 1);
    }

    if (current != -1) {

      key[current].drawButton(true, String(current + 1));

      String msg = "BTN_" + String(current + 1);
      sendUDP(msg);

      Serial.printf("Button %d pressed\n", current + 1);
    }
    lastPressed = current;
  }

  delay(20);
}