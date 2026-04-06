#include "display_hal.h"

#include <Arduino.h>
#include "pins.h"
#include "app_state.h"

static XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
static TFT_eSPI tft;

// ====================== TOUCH CALIBRATION ======================
static int TOUCH_MIN_X = 25;
static int TOUCH_MAX_X = 222;
static int TOUCH_MIN_Y = 31;
static int TOUCH_MAX_Y = 281;

static bool swapXY  = true;
static bool invertX = true;
static bool invertY = false;

void initDisplayHardware() {
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
}

void initTouchHardware() {
  ts.begin();
}

TFT_eSPI& display() {
  return tft;
}

XPT2046_Bitbang& touch() {
  return ts;
}

void screenLock() {
  if (screenMutex != NULL) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
  }
}

void screenUnlock() {
  if (screenMutex != NULL) {
    xSemaphoreGive(screenMutex);
  }
}

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