#include "ui_start.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>

#include "display_hal.h"
#include "app_state.h"
#include "ui_dev.h"
#include "ui_setup.h"

// ====================== BUTTONS ======================
static TFT_eSPI_Button btnMic;
static TFT_eSPI_Button btnSetup;
static TFT_eSPI_Button btnDev;

// ====================== LAYOUT ======================
static constexpr int NET_DOT_X = 220;
static constexpr int NET_DOT_Y = 14;
static constexpr int NET_DOT_R = 5;

static constexpr int MIC_DOT_X = 195;
static constexpr int MIC_DOT_Y = 185;
static constexpr int MIC_DOT_R = 5;

static constexpr int AUDIO_BOX_X = 10;
static constexpr int AUDIO_BOX_Y = 30;
static constexpr int AUDIO_BOX_W = 220;
static constexpr int AUDIO_BOX_H = 55;

static constexpr int MIC_BTN_X = 120;
static constexpr int MIC_BTN_Y = 180;
static constexpr int MIC_BTN_W = 120;
static constexpr int MIC_BTN_H = 70;

static constexpr int SETUP_BTN_X = 55;
static constexpr int SETUP_BTN_Y = 285;
static constexpr int SETUP_BTN_W = 80;
static constexpr int SETUP_BTN_H = 34;

static constexpr int DEV_BTN_X = 185;
static constexpr int DEV_BTN_Y = 285;
static constexpr int DEV_BTN_W = 80;
static constexpr int DEV_BTN_H = 34;

// ====================== STATE ======================
volatile int16_t g_startAudioLevel = 0;
volatile bool g_startAudioDirty = false;
volatile AudioBarMode g_audioBarMode = AUDIO_BAR_IDLE;

// ====================== HELPERS ======================
static void clearInnerBox(int x, int y, int w, int h)
{
    display().fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
}

static void drawButtonSafe(TFT_eSPI_Button &btn, bool inverted)
{
    screenLock();
    btn.drawButton(inverted);
    screenUnlock();
}

static void drawBoxTitle(int x, int y, const char *title)
{
    TFT_eSPI &tft = display();

    const int paddingX = 6;
    const int titleH = 12;
    const int titleY = y - (titleH / 2);

    int textW = strlen(title) * 6;
    int boxW = textW + 10;
    if (boxW < 36)
        boxW = 36;

    tft.fillRect(x + paddingX, titleY, boxW, titleH, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(x + paddingX + 4, titleY + 2);
    tft.print(title);
}

static const char *getAudioBarTitle()
{
    switch (g_audioBarMode)
    {
    case AUDIO_BAR_MIC:
        return "MIC LEVEL";
    case AUDIO_BAR_INCOMING:
        return "INCOMING";
    default:
        return "AUDIO";
    }
}

static uint16_t getNetworkStatusColor()
{
    if (wifiStatus != WIFI_CONNECTED)
        return TFT_WHITE;

    if (udpStatus != UDP_CONNECTED)
        return TFT_ORANGE;

    return TFT_GREEN;
}

static uint16_t getMicStatusColor()
{
    return micStreaming ? TFT_RED : TFT_WHITE;
}

static void initButtons()
{
    TFT_eSPI &tft = display();

    btnMic.initButton(
        &tft,
        MIC_BTN_X, MIC_BTN_Y, MIC_BTN_W, MIC_BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)"MIC",
        2);

    btnSetup.initButton(
        &tft,
        SETUP_BTN_X, SETUP_BTN_Y, SETUP_BTN_W, SETUP_BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)"SETUP",
        1);

    btnDev.initButton(
        &tft,
        DEV_BTN_X, DEV_BTN_Y, DEV_BTN_W, DEV_BTN_H,
        TFT_GREEN, TFT_BLACK, TFT_GREEN,
        (char *)"DEV",
        1);
}

static void drawAudioBarNoLock(int16_t volume)
{
    TFT_eSPI &tft = display();

    clearInnerBox(AUDIO_BOX_X, AUDIO_BOX_Y, AUDIO_BOX_W, AUDIO_BOX_H);
    drawBoxTitle(AUDIO_BOX_X, AUDIO_BOX_Y, getAudioBarTitle());

    const int barX = AUDIO_BOX_X + 10;
    const int barY = AUDIO_BOX_Y + 20;
    const int barW = 196;
    const int barH = 18;

    int clamped = constrain(volume, 0, 32767);
    int fillW = map(clamped, 0, 32767, 0, barW - 2);

    tft.drawRect(barX, barY, barW, barH, TFT_GREEN);
    tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, TFT_BLACK);
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, TFT_GREEN);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(barX, barY + 24);
    tft.printf("%5d", volume);
}

// ====================== PUBLIC ======================
void updateStartAudioLevel(int16_t volume, AudioBarMode mode)
{
    g_startAudioLevel = volume;
    g_audioBarMode = mode;
    g_startAudioDirty = true;
}

void resetStartAudioLevel()
{
    g_startAudioLevel = 0;
    g_audioBarMode = AUDIO_BAR_IDLE;
    g_startAudioDirty = true;
}

void drawStartConnectionStatus()
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillRect(NET_DOT_X - 8, NET_DOT_Y - 8, 16, 16, TFT_BLACK);
    tft.fillCircle(NET_DOT_X, NET_DOT_Y, NET_DOT_R, getNetworkStatusColor());
    screenUnlock();
}

void drawStartMicStatus()
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillRect(MIC_DOT_X - 8, MIC_DOT_Y - 8, 16, 16, TFT_BLACK);
    tft.fillCircle(MIC_DOT_X, MIC_DOT_Y, MIC_DOT_R, getMicStatusColor());
    screenUnlock();
}

void drawStartUI()
{
    TFT_eSPI &tft = display();

    screenLock();

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 8);
    tft.print("START");

    tft.drawRect(AUDIO_BOX_X, AUDIO_BOX_Y, AUDIO_BOX_W, AUDIO_BOX_H, TFT_GREEN);

    initButtons();
    btnMic.drawButton(false);
    btnSetup.drawButton(false);
    btnDev.drawButton(false);

    drawAudioBarNoLock(0);

    screenUnlock();

    drawStartConnectionStatus();
    drawStartMicStatus();
}

void taskStartUI(void *pvParameters)
{
    int16_t lastVolume = -1;
    int lastWifi = -1;
    int lastUdp = -1;
    bool lastMic = false;
    AudioBarMode lastMode = AUDIO_BAR_IDLE;

    while (true)
    {
        if (currentScreen == SCREEN_START)
        {
            int16_t volume = g_startAudioLevel;
            AudioBarMode mode = g_audioBarMode;

            if (g_startAudioDirty &&
                (abs(volume - lastVolume) > 500 || mode != lastMode))
            {
                screenLock();
                drawAudioBarNoLock(volume);
                screenUnlock();

                lastVolume = volume;
                lastMode = mode;
                g_startAudioDirty = false;
            }

            if (lastWifi != wifiStatus || lastUdp != udpStatus)
            {
                drawStartConnectionStatus();
                lastWifi = wifiStatus;
                lastUdp = udpStatus;
            }

            if (lastMic != micStreaming)
            {
                drawStartMicStatus();
                lastMic = micStreaming;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void handleStartTouch(uint16_t x, uint16_t y)
{
    if (btnMic.contains(x, y))
    {
        drawButtonSafe(btnMic, true);

        if (!micStreaming)
        {
            micStreaming = true;
            drawStartMicStatus();

            vTaskDelay(120 / portTICK_PERIOD_MS);
            drawButtonSafe(btnMic, false);
        }
        else
        {
            commitRequested = true;
            micStreaming = false;
            drawStartMicStatus();
            resetStartAudioLevel();

            vTaskDelay(120 / portTICK_PERIOD_MS);
            drawButtonSafe(btnMic, false);
        }

        return;
    }

    if (btnSetup.contains(x, y))
    {
        drawButtonSafe(btnSetup, true);
        vTaskDelay(120 / portTICK_PERIOD_MS);

        currentScreen = SCREEN_SETUP;
        drawConfigScreen();
        return;
    }

    if (btnDev.contains(x, y))
    {
        drawButtonSafe(btnDev, true);
        vTaskDelay(120 / portTICK_PERIOD_MS);

        currentScreen = SCREEN_DEV;
        drawMainUI();
        return;
    }
}