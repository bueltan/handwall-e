#include <Arduino.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>
#include <FS.h>               // ← Agregado para 'File'

TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button btn[40];      // botones del teclado

String inputText = "";        // Texto que escribe el usuario
const int maxLength = 25;

String keyLabels[40] = {      // ← Array con los textos de los botones
  "1","2","3","4","5","6","7","8","9","0",
  "Q","W","E","R","T","Y","U","I","O","P",
  "A","S","D","F","G","H","J","K","L","DEL",
  "Z","X","C","V","B","N","M"," ","ENTER",""
};

uint16_t t_x = 0, t_y = 0;

#define CALIBRATION_FILE "/calData.bin"

// ==================== DIBUJAR CAMPO DE TEXTO ====================
void drawInputBox() {
  tft.fillRect(10, 30, 220, 45, TFT_BLACK);
  tft.drawRect(10, 30, 220, 45, TFT_GREEN);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(20, 42);
  tft.print(inputText);
  if (inputText.length() < maxLength) tft.print("|");   // cursor simple
}

// ==================== DIBUJAR TECLADO ====================
void drawKeyboard() {
  int index = 0;
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 10; col++) {
      if (index >= 39) break;

      int x = 12 + col * 23;
      int y = 110 + row * 48;
      int w = 21;
      int h = 38;

      // Hacer el espacio más ancho
      if (row == 3 && col == 7) { w = 46; }

      btn[index].initButton(&tft, x + w/2, y + h/2, w, h,
                            TFT_WHITE, TFT_NAVY, TFT_WHITE,
                            (char*)keyLabels[index].c_str(), 1);

      btn[index].drawButton(false);
      index++;
    }
  }
}

// ==================== MANEJAR TOQUE ====================
void handleTouch() {
  if (!tft.getTouch(&t_x, &t_y)) return;

  for (int i = 0; i < 39; i++) {
    if (btn[i].contains(t_x, t_y)) {
      btn[i].press(true);
      btn[i].drawButton(true);        // efecto presionado

      String label = keyLabels[i];

      if (label == "DEL") {
        if (inputText.length() > 0) inputText.remove(inputText.length() - 1);
      }
      else if (label == "ENTER") {
        Serial.printf("Texto final: %s\n", inputText.c_str());
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(20, 80);
        tft.setTextSize(2);
        tft.setTextColor(TFT_GREEN);
        tft.println("Ingresado:");
        tft.println(inputText);
        delay(2500);
        tft.fillScreen(TFT_BLACK);
        drawInputBox();
        drawKeyboard();
        return;
      }
      else if (label == " ") {
        if (inputText.length() < maxLength) inputText += " ";
      }
      else {
        if (inputText.length() < maxLength) inputText += label;
      }

      drawInputBox();
      delay(120);                     // anti-rebote
      btn[i].drawButton(false);
    }
  }
}

// ==================== CALIBRACIÓN CON LITTLEFS ====================
void touch_calibrate() {
  uint16_t calData[5] = {0};

  if (!LittleFS.begin(true)) {          // true = formatOnFail
    Serial.println("LittleFS Mount Failed - Formateando...");
    return;
  }

  if (LittleFS.exists(CALIBRATION_FILE)) {
    File f = LittleFS.open(CALIBRATION_FILE, "r");
    if (f) {
      f.read((uint8_t*)calData, sizeof(calData));
      f.close();
      tft.setTouch(calData);
      Serial.println("Calibración cargada desde LittleFS");
      return;
    }
  }

  // Calibrar si no existe el archivo
  Serial.println("Por favor calibra el touch. Toca las cruces...");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(20, 100);
  tft.println("Calibrando...");

  tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

  // Guardar calibración
  File f = LittleFS.open(CALIBRATION_FILE, "w");
  if (f) {
    f.write((uint8_t*)calData, sizeof(calData));
    f.close();
    Serial.println("Calibración guardada en LittleFS");
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);        // Cambia a 0, 2 o 3 si la pantalla está invertida
  tft.fillScreen(TFT_BLACK);

  touch_calibrate();

  drawInputBox();
  drawKeyboard();

  Serial.println("Teclado virtual + Input listo!");
}

void loop() {
  handleTouch();
  delay(10);
}