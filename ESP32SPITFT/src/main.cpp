#include <Arduino.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <ctype.h>
#include <esp_system.h>

#include "pins.h"
#include "config.h"
#include "app_state.h"
#include "ui_config.h"
#include "storage_prefs.h"
#include "display_hal.h"
#include "network_udp.h"
#include "ui_dev.h"
#include "ui_setup.h"
#include "ui_start.h"
#include "audio_playback.h"

// ====================== GLOBAL BUFFERS ======================
uint8_t packet[sizeof(AudioHeader) + PACKET_SIZE];
int32_t i2sBuffer[BUFFER_LEN];
uint8_t udpPacket[PACKET_SIZE];

void initI2SMic();
void taskMicStream(void *pvParameters);
void taskTouch(void *pvParameters);

// ====================== HELPERS ======================

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

void printResetReason()
{
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.printf("Reset reason: %d\n", (int)reason);
}

// ====================== I2S MIC ======================
void initI2SMic()
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
        .bck_io_num = I2S_MIC_SCK,
        .ws_io_num = I2S_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SD};

    i2s_driver_install(I2S_MIC_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_MIC_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_MIC_PORT);
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
            initI2SMic();
            i2sReady = true;
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }

        if ((currentScreen != SCREEN_DEV && currentScreen != SCREEN_START) ||
            !micStreaming ||
            wifiStatus != WIFI_CONNECTED)
        {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t result = i2s_read(
            I2S_MIC_PORT,
            i2sBuffer,
            sizeof(i2sBuffer),
            &bytes_read,
            portMAX_DELAY);

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

        if (currentScreen == SCREEN_DEV)
        {
            updateVolumeLevel(maxVolume);

            if (millis() - lastPrint > 800)
            {
                lastPrint = millis();
                char logMsg[100];
                snprintf(
                    logMsg,
                    sizeof(logMsg),
                    "Queued seq=%lu smp=%d peak=%d",
                    (unsigned long)header.sequence,
                    samples,
                    maxVolume);
                drawLog(logMsg);
            }
        }
        else if (currentScreen == SCREEN_START)
        {
            updateStartAudioLevel(maxVolume, AUDIO_BAR_MIC);
        }

        vTaskDelay(1 / portTICK_PERIOD_MS);
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
            if (currentScreen == SCREEN_SETUP)
            {
                handleConfigTouch(x, y);
            }
            else if (currentScreen == SCREEN_START)
            {
                handleStartTouch(x, y);
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

// ====================== SETUP ======================
void setup()
{
    Serial.begin(115200);

    screenMutex = xSemaphoreCreateMutex();

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

    currentScreen = SCREEN_START;
    drawStartUI();
    delay(300);

    udpAudioQueue = xQueueCreate(20, sizeof(UdpPacket_t));
    udpControlQueue = xQueueCreate(10, sizeof(UdpPacket_t));

    if (udpAudioQueue == NULL || udpControlQueue == NULL)
    {
        Serial.println("Error creating UDP queues");
    }

    initI2SOut();
    resetPlaybackBuffer();
    resetOutputPacketJitter();

    if (!initOutputOpus())
    {
        Serial.println("Failed to init output Opus decoder");
    }

    connectWiFi();

    xTaskCreatePinnedToCore(taskVolumeUI, "VolumeUI", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskStartUI, "StartUI", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskTouch, "TouchTask", 6144, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(taskMicStream, "MicStream", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(taskUDP, "UDPTask", 9144, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(taskAudioPlayback, "AudioPlayback", 12288, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(udpRxTask, "UdpRx", 4096, NULL, 3, NULL, 1);
    Serial.println("System ready");
}

void loop()
{
}