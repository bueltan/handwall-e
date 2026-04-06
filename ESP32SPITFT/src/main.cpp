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
#include "ui_main.h"
#include "network_udp.h"
#include <esp_system.h>

// ====================== CONFIG KEYBOARD BUTTONS ======================
TFT_eSPI_Button keyBtn[MAX_BUTTONS];
String keyLabels[MAX_BUTTONS];

// ====================== GLOBAL BUFFERS ======================
uint8_t packet[sizeof(AudioHeader) + PACKET_SIZE];
int32_t i2sBuffer[BUFFER_LEN];
uint8_t udpPacket[PACKET_SIZE];

// ====================== FORWARD DECLARATIONS ======================
void advanceToMainScreen();
void loadStageIntoInput();

void drawConfigTitle(const String &title);
void drawConfigInputBox();
void drawStageProgress();
void redrawKeyRows();
void drawBottomRow();
void drawKeyboard();
void drawConfigScreenNoMutex();
void drawConfigScreen();

void initI2S();
void taskMicStream(void *pvParameters);
bool isStillPressingConfigButton(int buttonIndex, unsigned long holdTimeMs);
void handleConfigTouch(uint16_t x, uint16_t y);
void taskTouch(void *pvParameters);

// ====================== HELPERS ======================
bool shouldShowConnectButton()
{
    return (CURRENT_STAGE == 2 && inputText.length() > 0);
}

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
        {
            tft.fillRect(bx, y, barW, h, UI_BORDER_COLOR);
        }
        else
        {
            tft.drawRect(bx, y, barW, h, UI_BORDER_COLOR);
        }
    }
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
    index = drawRow(index, 2, 90,  r1, c1, 20, 30, 4);
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

void drawConfigScreen()
{
    TFT_eSPI &tft = display();

    screenLock();
    tft.fillScreen(UI_BG_COLOR);
    drawConfigTitle(currentTitle);
    drawConfigInputBox();
    drawStageProgress();
    drawKeyboard();
    screenUnlock();
}

void advanceToMainScreen()
{
    currentScreen = SCREEN_MAIN;
    drawMainUI();
    drawStatus("Ready");
}

// ====================== I2S ======================
void initI2S()
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 10,
        .dma_buf_len = BUFFER_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0};

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD};

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

// ====================== MIC TASK ======================
void taskMicStream(void *pvParameters)
{
    static bool i2sReady = false;
    int16_t maxVolume = 0;
    uint32_t lastPrint = 0;

    while (true)
    {
        if (!i2sReady)
        {
            initI2S();
            i2sReady = true;
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }

        if (currentScreen != SCREEN_MAIN || !micStreaming || wifiStatus != WIFI_CONNECTED)
        {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t result = i2s_read(I2S_PORT, i2sBuffer, sizeof(i2sBuffer), &bytes_read, portMAX_DELAY);

        if (result != ESP_OK || bytes_read == 0)
        {
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        int samples = bytes_read / sizeof(int32_t);
        if (samples != SAMPLES_PER_PACKET)
        {
            vTaskDelay(2 / portTICK_PERIOD_MS);
            continue;
        }

        maxVolume = 0;

        for (int i = 0; i < samples; i++)
        {
            int32_t raw = i2sBuffer[i];
            int16_t sample = (int16_t)(raw >> 12);

            if (abs(sample) > maxVolume)
                maxVolume = abs(sample);

            udpPacket[i * 2] = (uint8_t)(sample & 0xFF);
            udpPacket[i * 2 + 1] = (uint8_t)((sample >> 8) & 0xFF);
        }

        AudioHeader header;
        header.sequence = sequence++;

        memcpy(packet, &header, sizeof(AudioHeader));
        memcpy(packet + sizeof(AudioHeader), udpPacket, samples * 2);

        if (!micStreaming)
        {
            vTaskDelay(1 / portTICK_PERIOD_MS);
            continue;
        }

        enqueueUDPAudio(packet, sizeof(AudioHeader) + samples * 2);

        if (currentScreen == SCREEN_MAIN)
        {
            drawVolumeBar(maxVolume);

            if (millis() - lastPrint > 800)
            {
                lastPrint = millis();
                char logMsg[100];
                snprintf(logMsg, sizeof(logMsg), "Queued seq=%lu smp=%d peak=%d",
                         (unsigned long)header.sequence, samples, maxVolume);
                drawLog(logMsg);
            }
        }

        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

// ====================== CONFIG TOUCH ======================
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
                else
                {
                    if (inputText.length() > 0)
                    {
                        inputText.remove(inputText.length() - 1);
                        drawConfigInputBox();
                        drawBottomRow();
                    }
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

// ====================== TOUCH TASK ======================
void taskTouch(void *pvParameters)
{
    while (1)
    {
        uint16_t x, y;

        if (getTouchPoint(x, y))
        {
            if (currentScreen == SCREEN_CONFIG)
            {
                handleConfigTouch(x, y);
            }
            else
            {
                handleMainTouch(x, y);
            }
            waitTouchRelease();
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("Reset reason: %d\n", (int)reason);
}
// ====================== SETUP ======================
void setup()
{
    Serial.begin(115200);

    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);
    delay(100);
    printResetReason();
    initDisplayHardware();
    initTouchHardware();
    delay(100);

    TFT_eSPI &tft = display();
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(20, 20);
    tft.println("HANDWALL-E");
    delay(300);

    initStoragePrefs();
    loadSavedConfig();

    CURRENT_STAGE = 0;
    currentTitle = getStageTitle(CURRENT_STAGE);
    inputText = loadStageValue(0);
    currentScreen = SCREEN_CONFIG;
    drawConfigScreenNoMutex();
    delay(300);

    screenMutex = xSemaphoreCreateMutex();

    udpAudioQueue = xQueueCreate(20, sizeof(UdpPacket_t));
    udpControlQueue = xQueueCreate(10, sizeof(UdpPacket_t));

    if (udpAudioQueue == NULL || udpControlQueue == NULL)
    {
        Serial.println("Error creating UDP queues");
    }

    xTaskCreatePinnedToCore(taskTouch, "TouchTask", 6144, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(taskMicStream, "MicStream", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(taskUDP, "UDPTask", 6144, NULL, 3, NULL, 1);

    Serial.println("System ready");
}

// ====================== LOOP ======================
void loop()
{
    // Everything runs in FreeRTOS tasks
}