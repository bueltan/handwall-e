#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <ctype.h>
#include <Preferences.h>

Preferences prefs;

// ====================== TOUCH ======================
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
TFT_eSPI tft = TFT_eSPI();

const int MAX_BUTTONS = 50;
TFT_eSPI_Button btn[MAX_BUTTONS];
String keyLabels[MAX_BUTTONS];
const unsigned long DEL_HOLD_MS = 1000;

// ====================== TOUCH CALIBRATION ======================
int TOUCH_MIN_X = 25;
int TOUCH_MAX_X = 222;
int TOUCH_MIN_Y = 31;
int TOUCH_MAX_Y = 281;

bool swapXY = true;
bool invertX = true;
bool invertY = false;

// ====================== INPUT ======================
String inputText = "";
const uint8_t maxLength = 150;

uint16_t t_x = 0, t_y = 0;
bool capsLock = false;
bool symbolMode = false;

int CURRENT_STAGE = 0; // 0: WiFi Name, 1: WiFi Password, 2: Server IP
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
const uint16_t UI_BG_COLOR = TFT_BLACK;
const uint16_t UI_BORDER_COLOR = TFT_GREEN;
const uint16_t UI_TEXT_COLOR = TFT_GREEN;
const uint16_t KEY_FILL_COLOR = TFT_BLACK;
const uint16_t KEY_TEXT_COLOR = TFT_GREEN;
const uint16_t KEY_BORDER_COLOR = TFT_GREEN;

// ====================== PREFERENCES HELPERS ======================
String getStageKey(int stage)
{
  if (stage == 0) return "wifi_ssid";
  if (stage == 1) return "wifi_pass";
  return "server_ip";
}

String getStageTitle(int stage)
{
  if (stage == 0) return "WiFi Name:";
  if (stage == 1) return "WiFi Password:";
  return "Server IP:";
}

bool shouldShowConnectButton()
{
  return (CURRENT_STAGE == 2 && inputText.length() > 0);
}

// ====================== UI ======================
void drawTitle(const String &title)
{
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

void drawInputBox()
{
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

  if (visibleText.length() > maxVisible)
  {
    visibleText = visibleText.substring(visibleText.length() - maxVisible);
  }

  String line1 = "";
  String line2 = "";

  if (visibleText.length() <= maxCharsPerLine)
  {
    line1 = visibleText;
  }
  else
  {
    line1 = visibleText.substring(0, maxCharsPerLine);
    line2 = visibleText.substring(maxCharsPerLine);
  }

  tft.setCursor(boxX + 8, boxY + 8);
  tft.print(line1);

  tft.setCursor(boxX + 8, boxY + 24);
  tft.print(line2);

  if (inputText.length() < maxLength)
  {
    tft.print("|");
  }
}

void drawStageProgress()
{
  const int totalStages = 3;
  const int x = 2;
  const int y = 82;
  const int h = 3;
  const int gap = 4;
  const int totalW = 236;
  const int barW = (totalW - (gap * (totalStages - 1))) / totalStages;

  tft.fillRect(0, y - 1, 240, h + 2, UI_BG_COLOR);

  for (int i = 0; i < totalStages; i++)
  {
    int bx = x + i * (barW + gap);

    if (i <= CURRENT_STAGE)
    {
      tft.fillRect(bx, y, barW, h, UI_BORDER_COLOR);
    }
    else
    {
      tft.drawRect(bx, y, barW, h, UI_BORDER_COLOR);
    }
  }
}

// ====================== KEY FORMAT ======================
String formatKey(const char *key)
{
  String s = String(key);

  if (!symbolMode && s.length() == 1 && isalpha((unsigned char)s[0]))
  {
    char c = s[0];
    c = capsLock ? toupper(c) : tolower(c);
    return String(c);
  }

  return s;
}

// ====================== CLEAR LABELS ======================
void clearKeyLabels()
{
  for (int i = 0; i < MAX_BUTTONS; i++)
  {
    keyLabels[i] = "";
  }
}

// ====================== DRAW BUTTON ======================
void drawButtonAtIndex(int index, int x, int y, int w, int h,
                       const String &label,
                       uint16_t outline, uint16_t fill, uint16_t text,
                       bool inverted = false)
{
  if (index < 0 || index >= MAX_BUTTONS) return;

  btn[index].initButton(&tft, x + w / 2, y + h / 2, w, h,
                        outline, fill, text,
                        (char *)label.c_str(), 1);
  btn[index].drawButton(inverted);
  keyLabels[index] = label;
}

// ====================== GENERIC ROW ======================
int drawRow(int startIndex, int startX, int y,
            const char *keys[], int keyCount,
            int w, int h, int spacing)
{
  int x = startX;
  int i = startIndex;

  for (int k = 0; k < keyCount; k++)
  {
    if (i >= MAX_BUTTONS) break;

    String label = formatKey(keys[k]);
    drawButtonAtIndex(i, x, y, w, h, label,
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);
    x += w + spacing;
    i++;
  }

  return i;
}

// ====================== ACTIVE ROWS ======================
void getActiveRows(const char ***r1, int &c1,
                   const char ***r2, int &c2,
                   const char ***r3, int &c3,
                   const char ***r4, int &c4)
{
  if (!symbolMode)
  {
    *r1 = row1Text; c1 = row1TextCount;
    *r2 = row2Text; c2 = row2TextCount;
    *r3 = row3Text; c3 = row3TextCount;
    *r4 = row4Text; c4 = row4TextCount;
  }
  else
  {
    *r1 = row1Sym; c1 = row1SymCount;
    *r2 = row2Sym; c2 = row2SymCount;
    *r3 = row3Sym; c3 = row3SymCount;
    *r4 = row4Sym; c4 = row4SymCount;
  }
}

// ====================== REDRAW KEY ROWS ======================
void redrawKeyRows()
{
  tft.fillRect(0, 85, 240, 170, UI_BG_COLOR);

  const char **r1;
  const char **r2;
  const char **r3;
  const char **r4;
  int c1, c2, c3, c4;

  getActiveRows(&r1, c1, &r2, c2, &r3, c3, &r4, c4);

  int index = 0;
  index = drawRow(index, 2, 90, r1, c1, 20, 30, 4);
  index = drawRow(index, 2, 130, r2, c2, 20, 30, 4);
  index = drawRow(index, 2, 170, r3, c3, 20, 30, 4);
  drawRow(index, 2, 210, r4, c4, 20, 30, 4);
}

// ====================== BOTTOM ROW ======================
void drawBottomRow()
{
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

  if (shouldShowConnectButton())
  {
    drawButtonAtIndex(OK_INDEX, 158, y, 80, h, "CONNECT",
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

    keyLabels[OK_INDEX] = "CONNECT";
    keyLabels[MODE_INDEX] = "";
  }
  else
  {
    drawButtonAtIndex(OK_INDEX, 158, y, 34, h, "OK",
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

    String modeLabel = symbolMode ? "ABC" : "FN";
    drawButtonAtIndex(MODE_INDEX, 196, y, 42, h, modeLabel,
                      KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);

    keyLabels[OK_INDEX] = "ENTER";
  }
}

// ====================== FULL KEYBOARD ======================
void drawKeyboard()
{
  clearKeyLabels();
  redrawKeyRows();
  drawBottomRow();
}

// ====================== INITIAL UI ======================
void drawInitialUI()
{
  tft.fillScreen(UI_BG_COLOR);
  drawTitle(currentTitle);
  drawInputBox();
  drawStageProgress();
  drawKeyboard();
}

// ====================== TOUCH RAW -> SCREEN ======================
bool getTouchPoint(uint16_t &x, uint16_t &y)
{
  const int samples = 10;
  long sumX = 0;
  long sumY = 0;
  int valid = 0;

  for (int i = 0; i < samples; i++)
  {
    TouchPoint p = ts.getTouch();

    if (!(p.x == 0 && p.y == 0))
    {
      sumX += p.x;
      sumY += p.y;
      valid++;
    }
    delay(2);
  }

  if (valid < 4) return false;

  int rawX = sumX / valid;
  int rawY = sumY / valid;

  if (swapXY)
  {
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

// ====================== WAIT FOR RELEASE ======================
void waitTouchRelease()
{
  uint16_t rx, ry;
  unsigned long start = millis();

  while (millis() - start < 250)
  {
    if (!getTouchPoint(rx, ry)) break;
    delay(8);
  }
}

// ====================== BUTTON VISUAL FEEDBACK ======================
void flashButton(int i)
{
  if (i < 0 || i >= MAX_BUTTONS) return;
  btn[i].drawButton(true);
  delay(60);
  btn[i].drawButton(false);
}

// ====================== HOLD DETECTION ======================
bool isStillPressingButton(int buttonIndex, unsigned long holdTimeMs)
{
  unsigned long start = millis();
  uint16_t x, y;

  while (millis() - start < holdTimeMs)
  {
    if (!getTouchPoint(x, y))
    {
      return false;
    }

    if (!btn[buttonIndex].contains(x, y))
    {
      return false;
    }

    delay(10);
  }

  return true;
}

// ====================== STAGE HANDLING ======================
void loadStageIntoInput()
{
  currentTitle = getStageTitle(CURRENT_STAGE);
  inputText = prefs.getString(getStageKey(CURRENT_STAGE).c_str(), "");
  symbolMode = false;

  drawTitle(currentTitle);
  drawInputBox();
  drawStageProgress();
  redrawKeyRows();
  drawBottomRow();
}

void saveCurrentStageValue()
{
  prefs.putString(getStageKey(CURRENT_STAGE).c_str(), inputText);
}

// ====================== TOUCH HANDLER ======================
void handleTouch()
{
  if (!getTouchPoint(t_x, t_y)) return;

  for (int i = 0; i < MAX_BUTTONS; i++)
  {
    if (keyLabels[i] == "") continue;

    if (btn[i].contains(t_x, t_y))
    {
      String label = keyLabels[i];

      flashButton(i);

      if (label == "CAPS")
      {
        if (!symbolMode)
        {
          capsLock = !capsLock;
          redrawKeyRows();
          drawBottomRow();
        }
        waitTouchRelease();
        return;
      }
      else if (label == "FN" || label == "ABC")
      {
        if (!shouldShowConnectButton())
        {
          symbolMode = !symbolMode;
          redrawKeyRows();
          drawBottomRow();
        }
        waitTouchRelease();
        return;
      }
      else if (label == "DEL")
      {
        bool longPress = isStillPressingButton(i, DEL_HOLD_MS);

        if (longPress)
        {
          inputText = "";
          drawInputBox();
          drawBottomRow();
          Serial.println("DEL long press -> clear all text");
          waitTouchRelease();
          return;
        }
        else
        {
          if (inputText.length() > 0)
          {
            inputText.remove(inputText.length() - 1);
            drawInputBox();
            drawBottomRow();
          }
        }
      }
      else if (label == "ENTER")
      {
        Serial.println("Entered text -> " + inputText);

        saveCurrentStageValue();

        if (CURRENT_STAGE < 2)
        {
          CURRENT_STAGE++;
          loadStageIntoInput();
        }
        else
        {
          Serial.println("Configuration saved:");
          Serial.println("wifi_ssid = " + prefs.getString("wifi_ssid", ""));
          Serial.println("wifi_pass = " + prefs.getString("wifi_pass", ""));
          Serial.println("server_ip = " + prefs.getString("server_ip", ""));
        }

        waitTouchRelease();
        return;
      }
      else if (label == "SPACE")
      {
        if (inputText.length() < maxLength)
        {
          inputText += " ";
          drawInputBox();
          drawBottomRow();
        }
      }
      else if (label == "CONNECT")
      {
        saveCurrentStageValue();

        Serial.println("CONNECT pressed");
        Serial.println("wifi_ssid = " + prefs.getString("wifi_ssid", ""));
        Serial.println("wifi_pass = " + prefs.getString("wifi_pass", ""));
        Serial.println("server_ip = " + prefs.getString("server_ip", ""));

        waitTouchRelease();
        return;
      }
      else
      {
        if (inputText.length() < maxLength)
        {
          inputText += label;
          drawInputBox();
          drawBottomRow();
        }
      }

      waitTouchRelease();
      return;
    }
  }
}

// ====================== TOUCH DEBUG ======================
void debugTouch()
{
  static unsigned long lastPrint = 0;

  uint16_t x, y;
  if (getTouchPoint(x, y))
  {
    if (millis() - lastPrint > 120)
    {
      lastPrint = millis();
      Serial.print("SCREEN X: ");
      Serial.print(x);
      Serial.print("  SCREEN Y: ");
      Serial.println(y);
    }
  }
}

// ====================== SETUP ======================
void setup()
{
  Serial.begin(115200);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(0);
  ts.begin();

  prefs.begin("keyboard", false);

  if (prefs.getString("wifi_ssid", "") == "")
  {
    prefs.putString("wifi_ssid", "Galaxy M12 C9A6");
  }
  if (prefs.getString("wifi_pass", "") == "")
  {
    prefs.putString("wifi_pass", "zhof1469");
  }
  if (prefs.getString("server_ip", "") == "")
  {
    prefs.putString("server_ip", "10.230.143.13");
  }

  CURRENT_STAGE = 0;
  currentTitle = getStageTitle(CURRENT_STAGE);
  inputText = prefs.getString("wifi_ssid", "");

  drawInitialUI();

  Serial.println("Keyboard ready");
  Serial.println("Initial state: lowercase, symbols OFF, CAPS OFF");
}

// ====================== LOOP ======================
void loop()
{
  handleTouch();
  // debugTouch();
  delay(10);
}