#include "ui_main.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>

#include "display_hal.h"
#include "app_state.h"
#include "network_udp.h"
#include "config.h"

static TFT_eSPI_Button btnWifi;
static TFT_eSPI_Button btnMic;
static TFT_eSPI_Button btnCommit;
static TFT_eSPI_Button btnCancel;

// ====================== LAYOUT ======================
static constexpr int AUDIO_BOX_X = 10;
static constexpr int AUDIO_BOX_Y = 10;
static constexpr int AUDIO_BOX_W = 220;
static constexpr int AUDIO_BOX_H = 50;

static constexpr int STATUS_BOX_X = 10;
static constexpr int STATUS_BOX_Y = 65;
static constexpr int STATUS_BOX_W = 220;
static constexpr int STATUS_BOX_H = 24;

static constexpr int UDP_BOX_X = 10;
static constexpr int UDP_BOX_Y = 95;
static constexpr int UDP_BOX_W = 220;
static constexpr int UDP_BOX_H = 110;

static constexpr int LOG_BOX_X = 10;
static constexpr int LOG_BOX_Y = 235;
static constexpr int LOG_BOX_W = 220;
static constexpr int LOG_BOX_H = 18;

static constexpr int BTN_Y = 290;
static constexpr int BTN_W = 50;
static constexpr int BTN_H = 34;

static constexpr int BTN_WIFI_X   = 32;
static constexpr int BTN_MIC_X    = 84;
static constexpr int BTN_CANCEL_X = 156;
static constexpr int BTN_COMMIT_X = 208;

// ====================== HELPERS ======================
static void clearInnerBox(int x, int y, int w, int h)
{
    TFT_eSPI &tft = display();
    tft.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
}

static void drawBoxTitle(int x, int y, const char *title)
{
    TFT_eSPI &tft = display();

    const int paddingX = 6;
    const int titleH = 12;
    const int titleY = y - (titleH / 2);

    int textW = strlen(title) * 6;   // aprox para textSize(1)
    int boxW = textW + 10;

    if (boxW < 36)
    {
        boxW = 36;
    }

    tft.fillRect(x + paddingX, titleY, boxW, titleH, TFT_BLACK);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(x + paddingX + 4, titleY + 2);
    tft.print(title);
}

static void printTrimmedLine(int x, int y, int maxChars, const char *msg)
{
    TFT_eSPI &tft = display();

    char buffer[128];
    if (msg)
    {
        strncpy(buffer, msg, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
    }
    else
    {
        buffer[0] = '\0';
    }

    int len = strlen(buffer);
    if (len > maxChars)
    {
        if (maxChars > 3)
        {
            buffer[maxChars - 3] = '.';
            buffer[maxChars - 2] = '.';
            buffer[maxChars - 1] = '.';
            buffer[maxChars] = '\0';
        }
        else
        {
            buffer[maxChars] = '\0';
        }
    }

    tft.setCursor(x, y);
    tft.print(buffer);
}

static void initMicButton()
{
    TFT_eSPI &tft = display();

    btnMic.initButton(
        &tft,
        BTN_MIC_X, BTN_Y, BTN_W, BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)(micStreaming ? "MIC ON" : "MIC OFF"),
        FONT_SIZE_S);
}

static void drawBottomButtons()
{
    TFT_eSPI &tft = display();

    btnWifi.initButton(
        &tft,
        BTN_WIFI_X, BTN_Y, BTN_W, BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)"WiFi",
        FONT_SIZE_S);
    btnWifi.drawButton(false);

    initMicButton();
    btnMic.drawButton(false);

    btnCancel.initButton(
        &tft,
        BTN_CANCEL_X, BTN_Y, BTN_W, BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)"CANCEL",
        FONT_SIZE_S);
    btnCancel.drawButton(false);

    btnCommit.initButton(
        &tft,
        BTN_COMMIT_X, BTN_Y, BTN_W, BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)"COMMIT",
        FONT_SIZE_S);
    btnCommit.drawButton(false);
}

static void refreshMicButton()
{
    screenLock();
    initMicButton();
    btnMic.drawButton(false);
    screenUnlock();
}

// ====================== PUBLIC UI ======================
void drawMainUI()
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);

    tft.drawRect(AUDIO_BOX_X, AUDIO_BOX_Y, AUDIO_BOX_W, AUDIO_BOX_H, TFT_GREEN);
    drawBoxTitle(AUDIO_BOX_X, AUDIO_BOX_Y, "Audio Level");

    tft.drawRect(STATUS_BOX_X, STATUS_BOX_Y, STATUS_BOX_W, STATUS_BOX_H, TFT_GREEN);
    drawBoxTitle(STATUS_BOX_X, STATUS_BOX_Y, "Status");

    tft.drawRect(UDP_BOX_X, UDP_BOX_Y, UDP_BOX_W, UDP_BOX_H, TFT_GREEN);
    drawBoxTitle(UDP_BOX_X, UDP_BOX_Y, "Incoming UDP");

    tft.drawRect(LOG_BOX_X, LOG_BOX_Y, LOG_BOX_W, LOG_BOX_H, TFT_GREEN);
    drawBoxTitle(LOG_BOX_X, LOG_BOX_Y, "Log");

    drawBottomButtons();

    screenUnlock();

    drawStatus("Ready");
    drawIncomingUDP("No data");
    drawLog("System init");
}

void drawStatus(const char *msg)
{
    TFT_eSPI &tft = display();

    screenLock();
    clearInnerBox(STATUS_BOX_X, STATUS_BOX_Y, STATUS_BOX_W, STATUS_BOX_H);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(STATUS_BOX_X + 8, STATUS_BOX_Y + 8);
    tft.printf("W:%s U:%s M:%s",
               (wifiStatus == WIFI_CONNECTED) ? "OK" : "NO",
               (udpStatus == UDP_CONNECTED) ? "OK" : "NO",
               micStreaming ? "ON" : "OFF");
    screenUnlock();

    drawLog(msg);
}

void drawLog(const char *msg)
{
    TFT_eSPI &tft = display();

    screenLock();
    clearInnerBox(LOG_BOX_X, LOG_BOX_Y, LOG_BOX_W, LOG_BOX_H);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    printTrimmedLine(LOG_BOX_X + 6, LOG_BOX_Y + 5, 34, msg);

    screenUnlock();
}

void drawIncomingUDP(const char *msg)
{
    TFT_eSPI &tft = display();

    screenLock();
    clearInnerBox(UDP_BOX_X, UDP_BOX_Y, UDP_BOX_W, UDP_BOX_H);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);

    const int startX = UDP_BOX_X + 6;
    const int startY = UDP_BOX_Y + 18;
    const int lineStep = 16;
    const int maxCharsPerLine = 28;
    const int maxLines = 6;

    if (msg == nullptr || msg[0] == '\0')
    {
        tft.setCursor(startX, startY);
        tft.print("No data");
        screenUnlock();
        return;
    }

    char buffer[256];
    strncpy(buffer, msg, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    int len = strlen(buffer);
    int idx = 0;

    for (int line = 0; line < maxLines && idx < len; ++line)
    {
        char out[40];
        int outPos = 0;

        while (idx < len && buffer[idx] == ' ')
        {
            idx++;
        }

        while (idx < len && outPos < maxCharsPerLine)
        {
            if (buffer[idx] == '\n')
            {
                idx++;
                break;
            }
            out[outPos++] = buffer[idx++];
        }

        out[outPos] = '\0';

        if (idx < len && line == maxLines - 1 && outPos >= 3)
        {
            out[outPos - 3] = '.';
            out[outPos - 2] = '.';
            out[outPos - 1] = '.';
        }

        tft.setCursor(startX, startY + (line * lineStep));
        tft.print(out);
    }

    screenUnlock();
}

void drawVolumeBar(int16_t volume)
{
    TFT_eSPI &tft = display();

    screenLock();

    const int barX = AUDIO_BOX_X + 10;
    const int barY = AUDIO_BOX_Y + 20;
    const int barW = 196;
    const int barH = 20;

    int clamped = constrain(volume, 0, 32767);
    int fillW = map(clamped, 0, 32767, 0, barW - 2);

    tft.drawRect(barX, barY, barW, barH, TFT_GREEN);
    tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, TFT_BLACK);
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, TFT_GREEN);

    tft.fillRect(barX, barY + 28, 130, 12, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(barX, barY + 30);
    tft.printf("Vol: %5d", volume);

    screenUnlock();
}

// ====================== MAIN TOUCH ======================
void handleMainTouch(uint16_t x, uint16_t y)
{
    if (btnWifi.contains(x, y))
    {
        btnWifi.drawButton(true);
        connectWiFi();
        vTaskDelay(250 / portTICK_PERIOD_MS);
        btnWifi.drawButton(false);
        return;
    }

    if (btnMic.contains(x, y))
    {
        btnMic.drawButton(true);

        micStreaming = !micStreaming;
        refreshMicButton();
        drawStatus(micStreaming ? "Mic streaming ON" : "Mic streaming OFF");

        vTaskDelay(200 / portTICK_PERIOD_MS);
        return;
    }

    if (btnCommit.contains(x, y))
    {
        btnCommit.drawButton(true);

        micStreaming = false;
        refreshMicButton();

        commitRequested = true;
        drawStatus("Commit requested");

        vTaskDelay(200 / portTICK_PERIOD_MS);
        btnCommit.drawButton(false);
        return;
    }

    if (btnCancel.contains(x, y))
    {
        btnCancel.drawButton(true);

        micStreaming = false;
        refreshMicButton();

        cancelRequested = true;
        drawStatus("Cancel requested");

        vTaskDelay(200 / portTICK_PERIOD_MS);
        btnCancel.drawButton(false);
        return;
    }
}