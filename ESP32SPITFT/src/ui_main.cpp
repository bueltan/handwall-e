#include "ui_main.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "display_hal.h"
#include "app_state.h"
#include "network_udp.h"
#include "config.h"

static TFT_eSPI_Button btnWifi;
static TFT_eSPI_Button btnSend;
static TFT_eSPI_Button btnMic;
static TFT_eSPI_Button btnCommit;
static TFT_eSPI_Button btnCancel;

void drawMainUI()
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillScreen(TFT_BLACK);

    btnWifi.initButton(&tft, 60, 35, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"WiFi", FONT_SIZE_S);
    btnWifi.drawButton(false);

    btnSend.initButton(&tft, 180, 35, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"SEND", FONT_SIZE_S);
    btnSend.drawButton(false);

    btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"MIC OFF", FONT_SIZE_S);
    btnMic.drawButton(false);

    btnCancel.initButton(&tft, 120, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"CANCEL", FONT_SIZE_S);
    btnCancel.drawButton(false);

    btnCommit.initButton(&tft, 195, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"COMMIT", FONT_SIZE_S);
    btnCommit.drawButton(false);

    tft.drawRect(10, 70, 220, 80, TFT_GREEN);
    tft.setCursor(20, 80);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(FONT_SIZE_S);
    tft.println("Audio Level");

    tft.setCursor(20, 170);
    tft.println("Logs:");

    screenUnlock();
}

void drawStatus(const char *msg)
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillRect(15, 200, 220, 55, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(20, 210);
    tft.println(msg);

    tft.setCursor(20, 242);
    tft.printf("WiFi:%s UDP:%s Mic:%s",
               (wifiStatus == WIFI_CONNECTED) ? "OK" : "NO",
               (udpStatus == UDP_CONNECTED) ? "OK" : "NO",
               micStreaming ? "ON" : "OFF");
    screenUnlock();
}

void drawLog(const char *msg)
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillRect(20, 215, 200, 18, TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(20, 215);
    tft.println(msg);
    screenUnlock();
}

void drawVolumeBar(int16_t volume)
{
    TFT_eSPI &tft = display();

    screenLock();
    int barWidth = map(constrain(volume, 0, 32767), 0, 32767, 0, 200);
    tft.fillRect(20, 110, 200, 25, TFT_BLACK);
    tft.fillRect(20, 110, barWidth, 25, TFT_GREEN);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(20, 140);
    tft.setTextSize(FONT_SIZE_S);
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
        vTaskDelay(300 / portTICK_PERIOD_MS);
        btnWifi.drawButton(false);
        return;
    }

    if (btnSend.contains(x, y))
    {
        btnSend.drawButton(true);
        if (enqueueUDPControl("HELLO_FROM_ESP32 | t=" + String(millis())))
        {
            drawStatus("Queued HELLO");
        }
        else
        {
            drawStatus("Queue HELLO failed");
        }
        vTaskDelay(300 / portTICK_PERIOD_MS);
        btnSend.drawButton(false);
        return;
    }

    if (btnMic.contains(x, y))
    {
        TFT_eSPI &tft = display();

        btnMic.drawButton(true);
        micStreaming = !micStreaming;

        if (micStreaming)
        {
            btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"MIC ON", FONT_SIZE_S);
        }
        else
        {
            btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"MIC OFF", FONT_SIZE_S);
        }

        btnMic.drawButton(false);
        drawStatus(micStreaming ? "Audio streaming STARTED" : "Audio streaming STOPPED");
        vTaskDelay(250 / portTICK_PERIOD_MS);
        return;
    }

    if (btnCommit.contains(x, y))
    {
        TFT_eSPI &tft = display();

        btnCommit.drawButton(true);

        micStreaming = false;
        btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"MIC OFF", FONT_SIZE_S);
        btnMic.drawButton(false);

        commitRequested = true;
        drawStatus("Commit requested");

        vTaskDelay(200 / portTICK_PERIOD_MS);
        btnCommit.drawButton(false);
        return;
    }

    if (btnCancel.contains(x, y))
    {
        TFT_eSPI &tft = display();

        btnCancel.drawButton(true);

        micStreaming = false;
        btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char *)"MIC OFF", FONT_SIZE_S);
        btnMic.drawButton(false);

        cancelRequested = true;
        drawStatus("Cancel requested");

        vTaskDelay(200 / portTICK_PERIOD_MS);
        btnCancel.drawButton(false);
        return;
    }
}