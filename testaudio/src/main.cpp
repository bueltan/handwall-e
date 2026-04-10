#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>
#include "driver/i2s.h"

#define I2S_PORT I2S_NUM_0

// ===== PCM5102A pins =====
static const uint8_t PIN_I2S_BCK  = 4;
static const uint8_t PIN_I2S_WS   = 5;
static const uint8_t PIN_I2S_DOUT = 6;

// ===== Network config =====
const char* WIFI_SSID = "Galaxy M12 C9A6";
const char* WIFI_PASS = "zhof1469";
const char* SERVER_IP = "10.19.187.13";
const uint16_t SERVER_PORT = 9999;
const uint16_t LOCAL_PORT = 54321;

// ===== Audio config =====
static const uint32_t SAMPLE_RATE = 16000;
static const size_t UDP_RX_BUFFER_SIZE = 1472;   // Safe UDP payload size on LAN
static const size_t I2S_BLOCK_SAMPLES = 256;     // mono samples per write block
static const size_t PREBUFFER_SAMPLES = 3200;    // 200 ms at 16 kHz
static const size_t FIFO_SAMPLES = 16000 * 3;    // 3 seconds mono buffer

// ===== Task stack / priority =====
static const uint32_t TASK_STACK_WORDS = 4096;
static const UBaseType_t UDP_TASK_PRIORITY = 2;
static const UBaseType_t AUDIO_TASK_PRIORITY = 3;

// ===== Globals =====
WiFiUDP udp;

static uint8_t udpRxBuffer[UDP_RX_BUFFER_SIZE];
static int16_t stereoOutBuffer[I2S_BLOCK_SAMPLES * 2];
static int16_t audioFifo[FIFO_SAMPLES];

static volatile size_t fifoWriteIndex = 0;
static volatile size_t fifoReadIndex = 0;
static volatile size_t fifoCount = 0;

static portMUX_TYPE fifoMux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool streamRequested = false;
static volatile bool streamFinished = false;
static volatile bool playbackStarted = false;
static volatile bool serverEndReceived = false;

static volatile uint32_t droppedSamples = 0;
static volatile uint32_t receivedPackets = 0;
static volatile uint32_t receivedSamples = 0;

unsigned long lastPacketMs = 0;
unsigned long lastStreamRequestMs = 0;
unsigned long lastStatsMs = 0;

// =========================================================
// I2S
// =========================================================
void setupI2S()
{
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = 0;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = 256;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = PIN_I2S_BCK;
    pin_config.ws_io_num = PIN_I2S_WS;
    pin_config.data_out_num = PIN_I2S_DOUT;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}

// =========================================================
// WiFi / UDP
// =========================================================
void connectWiFi()
{
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi connected");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
}

bool startUdp()
{
    if (udp.begin(LOCAL_PORT))
    {
        Serial.printf("UDP listening on local port %u\n", LOCAL_PORT);
        return true;
    }

    Serial.println("Failed to start UDP");
    return false;
}

void clearAudioFifo()
{
    portENTER_CRITICAL(&fifoMux);
    fifoWriteIndex = 0;
    fifoReadIndex = 0;
    fifoCount = 0;
    portEXIT_CRITICAL(&fifoMux);
}

void requestStream()
{
    Serial.printf("Requesting stream from %s:%u\n", SERVER_IP, SERVER_PORT);

    udp.beginPacket(SERVER_IP, SERVER_PORT);
    udp.print("START");
    udp.endPacket();

    clearAudioFifo();

    streamRequested = true;
    streamFinished = false;
    playbackStarted = false;
    serverEndReceived = false;
    lastPacketMs = millis();
    lastStreamRequestMs = millis();
}

// =========================================================
// FIFO helpers
// =========================================================
size_t fifoAvailableSamples()
{
    size_t count;
    portENTER_CRITICAL(&fifoMux);
    count = fifoCount;
    portEXIT_CRITICAL(&fifoMux);
    return count;
}

void fifoPushSample(int16_t sample)
{
    portENTER_CRITICAL(&fifoMux);

    if (fifoCount < FIFO_SAMPLES)
    {
        audioFifo[fifoWriteIndex] = sample;
        fifoWriteIndex = (fifoWriteIndex + 1) % FIFO_SAMPLES;
        fifoCount++;
    }
    else
    {
        droppedSamples++;
    }

    portEXIT_CRITICAL(&fifoMux);
}

bool fifoPopSample(int16_t& sample)
{
    bool ok = false;

    portENTER_CRITICAL(&fifoMux);

    if (fifoCount > 0)
    {
        sample = audioFifo[fifoReadIndex];
        fifoReadIndex = (fifoReadIndex + 1) % FIFO_SAMPLES;
        fifoCount--;
        ok = true;
    }

    portEXIT_CRITICAL(&fifoMux);
    return ok;
}

// =========================================================
// UDP audio input
// =========================================================
void pushUdpPayloadToFifo(const uint8_t* data, size_t lenBytes)
{
    if (lenBytes < 2)
    {
        return;
    }

    size_t monoSamples = lenBytes / 2;

    for (size_t i = 0; i < monoSamples; ++i)
    {
        const size_t byteIndex = i * 2;

        // PCM16 little-endian
        int16_t sample = (int16_t)(
            ((uint16_t)data[byteIndex]) |
            ((uint16_t)data[byteIndex + 1] << 8)
        );

        fifoPushSample(sample);
    }

    receivedSamples += monoSamples;
}

void handleUdpPacket()
{
    int packetSize = udp.parsePacket();
    if (packetSize <= 0)
    {
        return;
    }

    if ((size_t)packetSize > sizeof(udpRxBuffer))
    {
        packetSize = sizeof(udpRxBuffer);
    }

    int len = udp.read(udpRxBuffer, packetSize);
    if (len <= 0)
    {
        return;
    }

    lastPacketMs = millis();
    receivedPackets++;

    if (len == 7 && memcmp(udpRxBuffer, "__END__", 7) == 0)
    {
        Serial.println("Stream finished by server");
        serverEndReceived = true;
        return;
    }

    pushUdpPayloadToFifo(udpRxBuffer, (size_t)len);
}

// =========================================================
// Audio output
// =========================================================
void writeStereoBlockToI2S(int16_t* stereoData, size_t monoSamples)
{
    size_t bytesWritten = 0;

    i2s_write(
        I2S_PORT,
        stereoData,
        monoSamples * 2 * sizeof(int16_t),
        &bytesWritten,
        portMAX_DELAY
    );
}

void fillStereoSilence(int16_t* outBuffer, size_t monoSamples)
{
    memset(outBuffer, 0, monoSamples * 2 * sizeof(int16_t));
}

void fillStereoFromFifo(int16_t* outBuffer, size_t monoSamples, size_t& actualSamples)
{
    actualSamples = 0;

    for (size_t i = 0; i < monoSamples; ++i)
    {
        int16_t sample = 0;
        bool ok = fifoPopSample(sample);
        if (!ok)
        {
            break;
        }

        outBuffer[i * 2 + 0] = sample;
        outBuffer[i * 2 + 1] = sample;
        actualSamples++;
    }

    // If underrun, pad the rest with silence
    for (size_t i = actualSamples; i < monoSamples; ++i)
    {
        outBuffer[i * 2 + 0] = 0;
        outBuffer[i * 2 + 1] = 0;
    }
}

// =========================================================
// Tasks
// =========================================================
void udpTask(void* pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        handleUdpPacket();

        // If no packet arrives for a while, ask again
        if (streamRequested && !serverEndReceived)
        {
            const unsigned long now = millis();
            if (now - lastPacketMs > 3000 && now - lastStreamRequestMs > 1000)
            {
                Serial.println("UDP timeout, re-requesting stream...");
                requestStream();
            }
        }

        // If server ended and buffer drained, request again after 2 seconds
        if (serverEndReceived && fifoAvailableSamples() == 0)
        {
            if (!streamFinished)
            {
                streamFinished = true;
                Serial.println("Playback buffer drained");
            }

            const unsigned long now = millis();
            if (now - lastStreamRequestMs > 2000)
            {
                requestStream();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void audioTask(void* pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        size_t available = fifoAvailableSamples();

        if (!playbackStarted)
        {
            if (available >= PREBUFFER_SAMPLES)
            {
                playbackStarted = true;
                Serial.printf("Playback started with prebuffer: %u samples\n", (unsigned)available);
            }
            else
            {
                fillStereoSilence(stereoOutBuffer, I2S_BLOCK_SAMPLES);
                writeStereoBlockToI2S(stereoOutBuffer, I2S_BLOCK_SAMPLES);
                continue;
            }
        }

        size_t actualSamples = 0;
        fillStereoFromFifo(stereoOutBuffer, I2S_BLOCK_SAMPLES, actualSamples);
        writeStereoBlockToI2S(stereoOutBuffer, I2S_BLOCK_SAMPLES);

        // If FIFO emptied after playback started, stop and wait for prebuffer again
        if (actualSamples == 0)
        {
            playbackStarted = false;
        }
    }
}

// =========================================================
// Setup / loop
// =========================================================
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("Init I2S...");
    setupI2S();

    connectWiFi();
    delay(200);

    if (!startUdp())
    {
        Serial.println("Cannot continue without UDP");
        while (true)
        {
            delay(1000);
        }
    }

    delay(100);
    requestStream();

    xTaskCreatePinnedToCore(
        udpTask,
        "udpTask",
        TASK_STACK_WORDS,
        NULL,
        UDP_TASK_PRIORITY,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        audioTask,
        "audioTask",
        TASK_STACK_WORDS,
        NULL,
        AUDIO_TASK_PRIORITY,
        NULL,
        1
    );

    Serial.println("Ready to receive PCM16 mono 16kHz UDP audio");
}

void loop()
{
    const unsigned long now = millis();

    if (now - lastStatsMs > 2000)
    {
        lastStatsMs = now;
        Serial.printf(
            "[STATS] fifo=%u samples | packets=%u | samples=%u | dropped=%u | playback=%s\n",
            (unsigned)fifoAvailableSamples(),
            (unsigned)receivedPackets,
            (unsigned)receivedSamples,
            (unsigned)droppedSamples,
            playbackStarted ? "ON" : "WAIT"
        );
    }

    delay(50);
}