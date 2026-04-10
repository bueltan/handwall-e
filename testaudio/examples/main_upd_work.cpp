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
// Define these as constants in your project.
extern const char* WIFI_SSID = "Galaxy M12 C9A6";
extern const char* WIFI_PASS = "zhof1469";
extern const char* SERVER_IP = "10.19.187.13";
extern const uint16_t SERVER_PORT = 9999;
extern const uint16_t LOCAL_PORT = 54321;

// ===== Audio config =====
static const uint32_t SAMPLE_RATE = 16000;
static const size_t UDP_RX_BUFFER_SIZE = 1024;       // bytes from server
static const size_t STEREO_BUFFER_SAMPLES = 512;     // output samples per channel

// Incoming audio from server: PCM16 mono little-endian
static uint8_t udpRxBuffer[UDP_RX_BUFFER_SIZE];

// Output to PCM5102A: stereo interleaved L/R
static int16_t stereoBuffer[STEREO_BUFFER_SAMPLES * 2];

WiFiUDP udp;

bool streamRequested = false;
bool streamFinished = false;
unsigned long lastPacketMs = 0;

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

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

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

void startUdp()
{
    if (udp.begin(LOCAL_PORT))
    {
        Serial.printf("UDP listening on local port %u\n", LOCAL_PORT);
    }
    else
    {
        Serial.println("Failed to start UDP");
    }
}

void requestStream()
{
    Serial.printf("Requesting stream from %s:%u\n", SERVER_IP, SERVER_PORT);

    udp.beginPacket(SERVER_IP, SERVER_PORT);
    udp.print("START");
    udp.endPacket();

    streamRequested = true;
    streamFinished = false;
    lastPacketMs = millis();
}

void writeSilenceMs(uint32_t durationMs)
{
    memset(stereoBuffer, 0, sizeof(stereoBuffer));
    size_t bytesWritten = 0;

    uint32_t totalMonoSamples = (SAMPLE_RATE * durationMs) / 1000;
    uint32_t sent = 0;

    while (sent < totalMonoSamples)
    {
        size_t blockSamples = STEREO_BUFFER_SAMPLES;
        if (sent + blockSamples > totalMonoSamples)
        {
            blockSamples = totalMonoSamples - sent;
        }

        i2s_write(
            I2S_PORT,
            stereoBuffer,
            blockSamples * 2 * sizeof(int16_t),
            &bytesWritten,
            portMAX_DELAY
        );

        sent += blockSamples;
    }
}

void playMonoUdpPayloadAsStereo(const uint8_t* data, size_t lenBytes)
{
    if (lenBytes < 2)
    {
        return;
    }

    // Each mono sample is 2 bytes (PCM16 little-endian)
    size_t monoSamples = lenBytes / 2;
    size_t offset = 0;
    size_t bytesWritten = 0;

    while (offset < monoSamples)
    {
        size_t blockSamples = monoSamples - offset;
        if (blockSamples > STEREO_BUFFER_SAMPLES)
        {
            blockSamples = STEREO_BUFFER_SAMPLES;
        }

        for (size_t i = 0; i < blockSamples; ++i)
        {
            size_t byteIndex = (offset + i) * 2;

            // Explicit little-endian decode:
            // low byte first, high byte second
            int16_t sample =
                (int16_t)((uint16_t)data[byteIndex] |
                         ((uint16_t)data[byteIndex + 1] << 8));

            // Duplicate mono sample to left and right
            stereoBuffer[i * 2 + 0] = sample;
            stereoBuffer[i * 2 + 1] = sample;
        }

        i2s_write(
            I2S_PORT,
            stereoBuffer,
            blockSamples * 2 * sizeof(int16_t),
            &bytesWritten,
            portMAX_DELAY
        );

        offset += blockSamples;
    }
}

void handleUdpAudio()
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

    if (len == 7 && memcmp(udpRxBuffer, "__END__", 7) == 0)
    {
        Serial.println("Stream finished by server");
        streamFinished = true;
        return;
    }

    playMonoUdpPayloadAsStereo(udpRxBuffer, (size_t)len);
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("Init I2S...");
    setupI2S();

    connectWiFi();
    delay(200);

    startUdp();
    delay(100);

    requestStream();

    Serial.println("Ready to receive PCM16 mono 16kHz UDP audio");
}

void loop()
{
    handleUdpAudio();

    // Optional: if stream ended, request again after 2 seconds
    if (streamFinished)
    {
        delay(2000);
        requestStream();
    }

    // Optional timeout protection
    if (streamRequested && !streamFinished)
    {
        if (millis() - lastPacketMs > 5000)
        {
            Serial.println("UDP timeout, writing short silence and retrying...");
            writeSilenceMs(200);
            requestStream();
        }
    }

    delay(1);
}