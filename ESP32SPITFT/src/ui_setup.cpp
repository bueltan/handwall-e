#include <Arduino.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <ctype.h>

#include "pins.h"
#include "config.h"
#include "app_state.h"
#include "ui_config.h"
#include "storage_prefs.h"
#include "display_hal.h"
#include "ui_dev.h"
#include "network_udp.h"
#include "ui_setup.h"
#include "ui_start.h"

TFT_eSPI_Button keyBtn[MAX_BUTTONS];
String keyLabels[MAX_BUTTONS];

// ====================== HELPERS ======================

bool isStillPressingConfigButton(int buttonIndex, unsigned long holdTimeMs)
{
    unsigned long start = millis();
    uint16_t x, y;

    while (millis() - start < holdTimeMs)
    {
        if (!getTouchPoint(x, y))
            return false;
        if (!keyBtn[buttonIndex].contains(x, y))
            return false;
        delay(10);
    }

    return true;
}

bool shouldShowConnectButton()
{
    return (CURRENT_STAGE == 2 && inputText.length() > 0);
}

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

void clearKeyLabels()
{
    for (int i = 0; i < MAX_BUTTONS; i++)
    {
        keyLabels[i] = "";
    }
}

// ====================== DRAW HELPERS ======================

void drawButtonAtIndex(int index, int x, int y, int w, int h,
                       const String &label,
                       uint16_t outline, uint16_t fill, uint16_t text,
                       bool inverted = false)
{
    TFT_eSPI &tft = display();

    if (index < 0 || index >= MAX_BUTTONS)
        return;

    keyBtn[index].initButton(&tft, x + w / 2, y + h / 2, w, h,
                             outline, fill, text,
                             (char *)label.c_str(), 1);
    keyBtn[index].drawButton(inverted);
    keyLabels[index] = label;
}

int drawRow(int startIndex, int startX, int y,
            const char *keys[], int keyCount,
            int w, int h, int spacing)
{
    int x = startX;
    int i = startIndex;

    for (int k = 0; k < keyCount; k++)
    {
        if (i >= MAX_BUTTONS)
            break;

        String label = formatKey(keys[k]);
        drawButtonAtIndex(i, x, y, w, h, label,
                          KEY_BORDER_COLOR, KEY_FILL_COLOR, KEY_TEXT_COLOR, false);
        x += w + spacing;
        i++;
    }

    return i;
}

// ====================== CONFIG UI ======================

void drawConfigTitle(const String &title)
{
    TFT_eSPI &tft = display();

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

void drawConfigInputBox()
{
    TFT_eSPI &tft = display();

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

    if (inputText.length() < MAX_INPUT_LENGTH)
    {
        tft.print("|");
    }
}

void drawStageProgress()
{
    TFT_eSPI &tft = display();

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
            tft.fillRect(bx, y, barW, h, UI_BORDER_COLOR);
        else
            tft.drawRect(bx, y, barW, h, UI_BORDER_COLOR);
    }
}

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

void redrawKeyRows()
{
    TFT_eSPI &tft = display();

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

void drawBottomRow()
{
    TFT_eSPI &tft = display();

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

void drawKeyboard()
{
    clearKeyLabels();
    redrawKeyRows();
    drawBottomRow();
}

void drawConfigScreenNoMutex()
{
    TFT_eSPI &tft = display();

    tft.fillScreen(UI_BG_COLOR);
    drawConfigTitle(currentTitle);
    drawConfigInputBox();
    drawStageProgress();
    drawKeyboard();
}

// ====================== STAGE FLOW ======================

void loadStageIntoInput()
{
    currentTitle = getStageTitle(CURRENT_STAGE);
    inputText = loadStageValue(CURRENT_STAGE);
    symbolMode = false;

    drawConfigTitle(currentTitle);
    drawConfigInputBox();
    drawStageProgress();
    redrawKeyRows();
    drawBottomRow();
}

void advanceToMainScreen()
{
    currentScreen = SCREEN_START;
    drawStartUI();
    connectWiFi();
}

// ====================== TOUCH ======================

void handleConfigTouch(uint16_t x, uint16_t y)
{
    for (int i = 0; i < MAX_BUTTONS; i++)
    {
        if (keyLabels[i] == "")
            continue;

        if (keyBtn[i].contains(x, y))
        {
            String label = keyLabels[i];

            keyBtn[i].drawButton(true);
            delay(60);
            keyBtn[i].drawButton(false);

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
                bool longPress = isStillPressingConfigButton(i, DEL_HOLD_MS);

                if (longPress)
                {
                    inputText = "";
                    drawConfigInputBox();
                    drawBottomRow();
                    waitTouchRelease();
                    return;
                }
                else if (inputText.length() > 0)
                {
                    inputText.remove(inputText.length() - 1);
                    drawConfigInputBox();
                    drawBottomRow();
                }
            }
            else if (label == "ENTER")
            {
                saveCurrentStageValue();

                if (CURRENT_STAGE < 2)
                {
                    CURRENT_STAGE++;
                    loadStageIntoInput();
                }
                else
                {
                    advanceToMainScreen();
                }

                waitTouchRelease();
                return;
            }
            else if (label == "SPACE")
            {
                if (inputText.length() < MAX_INPUT_LENGTH)
                {
                    inputText += " ";
                    drawConfigInputBox();
                    drawBottomRow();
                }
            }
            else if (label == "CONNECT")
            {
                saveCurrentStageValue();
                advanceToMainScreen();
                waitTouchRelease();
                return;
            }
            else
            {
                if (inputText.length() < MAX_INPUT_LENGTH)
                {
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