#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>
#include "driver/i2s.h"

#define I2S_PORT I2S_NUM_0

// =========================================================
// Logging
// =========================================================
#define LOG_BOOT(fmt, ...)     Serial.printf("[BOOT] " fmt "\n", ##__VA_ARGS__)
#define LOG_NET(fmt, ...)      Serial.printf("[NET] " fmt "\n", ##__VA_ARGS__)
#define LOG_STREAM(fmt, ...)   Serial.printf("[STREAM] " fmt "\n", ##__VA_ARGS__)
#define LOG_RX(fmt, ...)       Serial.printf("[RX] " fmt "\n", ##__VA_ARGS__)
#define LOG_FIFO(fmt, ...)     Serial.printf("[FIFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_PLAY(fmt, ...)     Serial.printf("[PLAYBACK] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)     Serial.printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)      Serial.printf("[ERR] " fmt "\n", ##__VA_ARGS__)
#define LOG_STATS(fmt, ...)    Serial.printf("[STATS] " fmt "\n", ##__VA_ARGS__)

// ===== PCM5102A pins =====
static const uint8_t PIN_I2S_BCK  = 4;
static const uint8_t PIN_I2S_WS   = 5;
static const uint8_t PIN_I2S_DOUT = 6;

// ===== Network config =====
const char* WIFI_SSID = "MOVISTAR-WIFI6-B700";
const char* WIFI_PASS = "uDRsVe6jpvSfSCiQ9wPL";
const char* SERVER_IP = "192.168.1.35";
const uint16_t SERVER_PORT = 9999;
const uint16_t LOCAL_PORT = 54321;

// ===== Audio config =====
static const uint32_t SAMPLE_RATE = 16000;
static const size_t UDP_RX_BUFFER_SIZE = 1472;   // Safe UDP payload size on LAN
static const size_t I2S_BLOCK_SAMPLES = 256;     // mono samples per write block
static const size_t PREBUFFER_SAMPLES = 9600;    // 200 ms at 16 kHz
static const size_t FIFO_SAMPLES = 16000 * 3;    // 3 seconds mono buffer

// ===== Task stack / priority =====
static const uint32_t TASK_STACK_WORDS = 4096;
static const UBaseType_t UDP_TASK_PRIORITY = 2;
static const UBaseType_t AUDIO_TASK_PRIORITY = 3;

// =========================================================
// Globals
// =========================================================
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
static volatile uint32_t streamRequestCount = 0;
static volatile uint32_t playbackStartCount = 0;
static volatile uint32_t playbackUnderrunCount = 0;
static volatile uint32_t serverEndCount = 0;
static volatile uint32_t rxTooSmallCount = 0;
static volatile uint32_t udpTimeoutCount = 0;
static volatile uint32_t fifoResetCount = 0;

static bool firstPacketLoggedForCurrentStream = false;

unsigned long lastPacketMs = 0;
unsigned long lastStreamRequestMs = 0;
unsigned long lastStatsMs = 0;
unsigned long lastRxLogMs = 0;
unsigned long lastFifoHighWaterLogMs = 0;

// =========================================================
// Helpers
// =========================================================
const char* onOff(bool value)
{
    return value ? "ON" : "OFF";
}

const char* yesNo(bool value)
{
    return value ? "YES" : "NO";
}

size_t fifoAvailableSamplesNoLock()
{
    return fifoCount;
}

size_t fifoAvailableSamples()
{
    size_t count;
    portENTER_CRITICAL(&fifoMux);
    count = fifoCount;
    portEXIT_CRITICAL(&fifoMux);
    return count;
}

size_t fifoFreeSamples()
{
    size_t used = fifoAvailableSamples();
    return (used <= FIFO_SAMPLES) ? (FIFO_SAMPLES - used) : 0;
}

unsigned long fifoBufferedMs()
{
    return (unsigned long)((fifoAvailableSamples() * 1000UL) / SAMPLE_RATE);
}

// =========================================================
// I2S
// =========================================================
void setupI2S()
{
    LOG_BOOT("Configuring I2S output...");
    LOG_BOOT("I2S pins | BCK=%u | WS=%u | DOUT=%u",
             (unsigned)PIN_I2S_BCK,
             (unsigned)PIN_I2S_WS,
             (unsigned)PIN_I2S_DOUT);

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

    LOG_BOOT("I2S ready | sample_rate=%u | dma_buf_count=%d | dma_buf_len=%d",
             (unsigned)SAMPLE_RATE,
             i2s_config.dma_buf_count,
             i2s_config.dma_buf_len);
}

// =========================================================
// WiFi / UDP
// =========================================================
void connectWiFi()
{
    LOG_NET("Connecting to WiFi | ssid='%s'", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t dots = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
        dots++;
        if ((dots % 20) == 0)
        {
            Serial.println();
            LOG_NET("Still waiting for WiFi connection...");
        }
    }

    Serial.println();
    LOG_NET("WiFi connected");
    LOG_NET("ESP32 IP=%s | RSSI=%d dBm",
            WiFi.localIP().toString().c_str(),
            WiFi.RSSI());
}

bool startUdp()
{
    LOG_NET("Starting UDP listener | local_port=%u", (unsigned)LOCAL_PORT);

    if (udp.begin(LOCAL_PORT))
    {
        LOG_NET("UDP listening OK | local_port=%u", (unsigned)LOCAL_PORT);
        return true;
    }

    LOG_ERR("Failed to start UDP listener");
    return false;
}

void clearAudioFifo()
{
    size_t beforeCount = 0;

    portENTER_CRITICAL(&fifoMux);
    beforeCount = fifoCount;
    fifoWriteIndex = 0;
    fifoReadIndex = 0;
    fifoCount = 0;
    portEXIT_CRITICAL(&fifoMux);

    fifoResetCount++;

    LOG_FIFO("FIFO cleared | previous_samples=%u | previous_ms=%lu | reset_count=%u",
             (unsigned)beforeCount,
             (unsigned long)((beforeCount * 1000UL) / SAMPLE_RATE),
             (unsigned)fifoResetCount);
}

void requestStream()
{
    streamRequestCount++;

    LOG_STREAM("Sending START request | request_id=%u | server=%s:%u",
               (unsigned)streamRequestCount,
               SERVER_IP,
               (unsigned)SERVER_PORT);

    udp.beginPacket(SERVER_IP, SERVER_PORT);
    udp.print("START");
    udp.endPacket();

    clearAudioFifo();

    streamRequested = true;
    streamFinished = false;
    playbackStarted = false;
    serverEndReceived = false;
    firstPacketLoggedForCurrentStream = false;

    lastPacketMs = millis();
    lastStreamRequestMs = millis();

    LOG_STREAM("State reset after START | playback=%s | end_received=%s | fifo=%u",
               onOff(playbackStarted),
               yesNo(serverEndReceived),
               (unsigned)fifoAvailableSamples());
}

// =========================================================
// FIFO helpers
// =========================================================
void fifoPushSample(int16_t sample)
{
    bool dropped = false;
    size_t countAfter = 0;

    portENTER_CRITICAL(&fifoMux);

    if (fifoCount < FIFO_SAMPLES)
    {
        audioFifo[fifoWriteIndex] = sample;
        fifoWriteIndex = (fifoWriteIndex + 1) % FIFO_SAMPLES;
        fifoCount++;
        countAfter = fifoCount;
    }
    else
    {
        droppedSamples++;
        dropped = true;
        countAfter = fifoCount;
    }

    portEXIT_CRITICAL(&fifoMux);

    if (dropped)
    {
        static unsigned long lastDropLogMs = 0;
        unsigned long now = millis();
        if (now - lastDropLogMs > 1000)
        {
            lastDropLogMs = now;
            LOG_WARN("FIFO full, dropping incoming audio | dropped_samples=%u | fifo=%u/%u",
                     (unsigned)droppedSamples,
                     (unsigned)countAfter,
                     (unsigned)FIFO_SAMPLES);
        }
    }
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
        rxTooSmallCount++;
        LOG_WARN("Ignoring tiny UDP payload | len=%u | tiny_count=%u",
                 (unsigned)lenBytes,
                 (unsigned)rxTooSmallCount);
        return;
    }

    size_t monoSamples = lenBytes / 2;
    size_t fifoBefore = fifoAvailableSamples();

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

    size_t fifoAfter = fifoAvailableSamples();
    unsigned long now = millis();

    if (!firstPacketLoggedForCurrentStream)
    {
        firstPacketLoggedForCurrentStream = true;
        LOG_RX("First audio packet for current stream | bytes=%u | mono_samples=%u | fifo_before=%u | fifo_after=%u",
               (unsigned)lenBytes,
               (unsigned)monoSamples,
               (unsigned)fifoBefore,
               (unsigned)fifoAfter);
    }

    if (now - lastRxLogMs > 1000)
    {
        lastRxLogMs = now;
        LOG_RX("Audio packet flow | last_bytes=%u | last_samples=%u | fifo=%u samples (%lu ms) | packets=%u",
               (unsigned)lenBytes,
               (unsigned)monoSamples,
               (unsigned)fifoAfter,
               (unsigned long)((fifoAfter * 1000UL) / SAMPLE_RATE),
               (unsigned)receivedPackets);
    }

    if (fifoAfter >= PREBUFFER_SAMPLES && fifoBefore < PREBUFFER_SAMPLES)
    {
        LOG_FIFO("Prebuffer threshold reached | fifo=%u samples (%lu ms) | threshold=%u",
                 (unsigned)fifoAfter,
                 (unsigned long)((fifoAfter * 1000UL) / SAMPLE_RATE),
                 (unsigned)PREBUFFER_SAMPLES);
    }

    if (fifoAfter > (FIFO_SAMPLES * 8 / 10))
    {
        if (now - lastFifoHighWaterLogMs > 1000)
        {
            lastFifoHighWaterLogMs = now;
            LOG_WARN("FIFO high water level | fifo=%u/%u samples (%lu ms buffered)",
                     (unsigned)fifoAfter,
                     (unsigned)FIFO_SAMPLES,
                     (unsigned long)((fifoAfter * 1000UL) / SAMPLE_RATE));
        }
    }
}

void handleUdpPacket()
{
    int packetSize = udp.parsePacket();
    if (packetSize <= 0)
    {
        return;
    }

    IPAddress remoteIp = udp.remoteIP();
    uint16_t remotePort = udp.remotePort();

    if ((size_t)packetSize > sizeof(udpRxBuffer))
    {
        LOG_WARN("Incoming UDP packet larger than buffer, truncating | packet_size=%d | max=%u | from=%s:%u",
                 packetSize,
                 (unsigned)sizeof(udpRxBuffer),
                 remoteIp.toString().c_str(),
                 (unsigned)remotePort);
        packetSize = sizeof(udpRxBuffer);
    }

    int len = udp.read(udpRxBuffer, packetSize);
    if (len <= 0)
    {
        LOG_WARN("UDP packet parse succeeded but read returned len=%d", len);
        return;
    }

    lastPacketMs = millis();
    receivedPackets++;

    if (len == 7 && memcmp(udpRxBuffer, "__END__", 7) == 0)
    {
        serverEndReceived = true;
        serverEndCount++;

        LOG_STREAM("Server sent end marker | end_count=%u | fifo=%u samples (%lu ms) | playback=%s",
                   (unsigned)serverEndCount,
                   (unsigned)fifoAvailableSamples(),
                   fifoBufferedMs(),
                   onOff(playbackStarted));
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

    const size_t expectedBytes = monoSamples * 2 * sizeof(int16_t);
    if (bytesWritten != expectedBytes)
    {
        LOG_WARN("i2s_write partial write | expected=%u | written=%u",
                 (unsigned)expectedBytes,
                 (unsigned)bytesWritten);
    }
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

    LOG_BOOT("udpTask started");

    for (;;)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            static bool wifiWaitLogged = false;
            if (!wifiWaitLogged)
            {
                wifiWaitLogged = true;
                LOG_WARN("udpTask waiting for WiFi reconnection...");
            }

            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        handleUdpPacket();

        if (streamRequested && !serverEndReceived)
        {
            const unsigned long now = millis();
            const unsigned long silenceMs = now - lastPacketMs;

            if (silenceMs > 3000 && now - lastStreamRequestMs > 1000)
            {
                udpTimeoutCount++;
                LOG_WARN("UDP timeout detected, re-requesting stream | silence_ms=%lu | timeout_count=%u | fifo=%u | playback=%s",
                         silenceMs,
                         (unsigned)udpTimeoutCount,
                         (unsigned)fifoAvailableSamples(),
                         onOff(playbackStarted));
                requestStream();
            }
        }

        if (serverEndReceived && fifoAvailableSamples() == 0)
        {
            if (!streamFinished)
            {
                streamFinished = true;
                LOG_PLAY("FIFO drained after server end | playback=%s | next_restart_allowed_in=2000ms",
                         onOff(playbackStarted));
            }

            const unsigned long now = millis();
            if (now - lastStreamRequestMs > 2000)
            {
                LOG_STREAM("Restarting stream after end+drain condition");
                requestStream();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void audioTask(void* pvParameters)
{
    (void)pvParameters;

    LOG_BOOT("audioTask started");

    bool waitingForPrebufferLogged = false;

    for (;;)
    {
        size_t available = fifoAvailableSamples();

        if (!playbackStarted)
        {
            if (available >= PREBUFFER_SAMPLES)
            {
                playbackStarted = true;
                playbackStartCount++;
                waitingForPrebufferLogged = false;

                LOG_PLAY("Playback started | start_count=%u | prebuffer=%u samples (%lu ms)",
                         (unsigned)playbackStartCount,
                         (unsigned)available,
                         (unsigned long)((available * 1000UL) / SAMPLE_RATE));
            }
            else
            {
                if (!waitingForPrebufferLogged)
                {
                    waitingForPrebufferLogged = true;
                    LOG_PLAY("Waiting for prebuffer | current=%u | target=%u | buffered_ms=%lu",
                             (unsigned)available,
                             (unsigned)PREBUFFER_SAMPLES,
                             (unsigned long)((available * 1000UL) / SAMPLE_RATE));
                }

                fillStereoSilence(stereoOutBuffer, I2S_BLOCK_SAMPLES);
                writeStereoBlockToI2S(stereoOutBuffer, I2S_BLOCK_SAMPLES);
                continue;
            }
        }

        size_t actualSamples = 0;
        fillStereoFromFifo(stereoOutBuffer, I2S_BLOCK_SAMPLES, actualSamples);
        writeStereoBlockToI2S(stereoOutBuffer, I2S_BLOCK_SAMPLES);

        if (actualSamples == 0)
        {
            playbackUnderrunCount++;
            playbackStarted = false;
            waitingForPrebufferLogged = false;

            LOG_WARN("Playback underrun: FIFO empty during active playback | underrun_count=%u | server_end=%s | stream_requested=%s",
                     (unsigned)playbackUnderrunCount,
                     yesNo(serverEndReceived),
                     yesNo(streamRequested));
        }
        else if (actualSamples < I2S_BLOCK_SAMPLES)
        {
            LOG_WARN("Partial audio block from FIFO | requested=%u | actual=%u | padded_with_silence=%u | fifo_remaining=%u",
                     (unsigned)I2S_BLOCK_SAMPLES,
                     (unsigned)actualSamples,
                     (unsigned)(I2S_BLOCK_SAMPLES - actualSamples),
                     (unsigned)fifoAvailableSamples());
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

    LOG_BOOT("ESP32 UDP PCM player starting...");
    LOG_BOOT("Audio config | sample_rate=%u | i2s_block_samples=%u | prebuffer_samples=%u | fifo_samples=%u",
             (unsigned)SAMPLE_RATE,
             (unsigned)I2S_BLOCK_SAMPLES,
             (unsigned)PREBUFFER_SAMPLES,
             (unsigned)FIFO_SAMPLES);

    LOG_BOOT("Network config | server=%s:%u | local_port=%u",
             SERVER_IP,
             (unsigned)SERVER_PORT,
             (unsigned)LOCAL_PORT);

    setupI2S();
    connectWiFi();
    delay(200);

    if (!startUdp())
    {
        LOG_ERR("Cannot continue without UDP");
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
        1
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

    LOG_BOOT("System ready | expecting PCM16 mono 16kHz over UDP");
}

void loop()
{
    const unsigned long now = millis();

    if (now - lastStatsMs > 2000)
    {
        lastStatsMs = now;

        LOG_STATS("fifo=%u/%u samples (%lu ms) | free=%u | packets=%u | samples=%u | dropped=%u | playback=%s | stream_requested=%s | server_end=%s | starts=%u | underruns=%u | timeouts=%u",
                  (unsigned)fifoAvailableSamples(),
                  (unsigned)FIFO_SAMPLES,
                  fifoBufferedMs(),
                  (unsigned)fifoFreeSamples(),
                  (unsigned)receivedPackets,
                  (unsigned)receivedSamples,
                  (unsigned)droppedSamples,
                  onOff(playbackStarted),
                  yesNo(streamRequested),
                  yesNo(serverEndReceived),
                  (unsigned)playbackStartCount,
                  (unsigned)playbackUnderrunCount,
                  (unsigned)udpTimeoutCount);
    }

    delay(50);
}