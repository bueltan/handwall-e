#include <Arduino.h>
#include <math.h>
#include "driver/i2s.h"

#define I2S_PORT I2S_NUM_0

// ===== PCM5102A pins =====
// Change these if you need other GPIOs.
static const uint8_t PIN_I2S_BCK  = 4;   // PCM5102A BCK
static const uint8_t PIN_I2S_WS   = 5;   // PCM5102A LRCK / WS / LCK
static const uint8_t PIN_I2S_DOUT = 6;   // PCM5102A DIN

// ===== Audio config =====
static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BUFFER_SAMPLES   = 256;
static const int16_t AMPLITUDE        = 10000;

static int16_t audioBuffer[BUFFER_SAMPLES * 2]; // stereo interleaved

void setupI2S()
{
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = 0;
    i2s_config.dma_buf_count = 6;
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

void playTone(bool leftOn, bool rightOn, float freqHz, uint32_t durationMs, uint8_t volumePercent)
{
    size_t bytesWritten = 0;
    float phase = 0.0f;
    const float phaseStep = 2.0f * PI * freqHz / SAMPLE_RATE;

    if (volumePercent > 100)
    {
        volumePercent = 100;
    }

    uint32_t totalSamples = (SAMPLE_RATE * durationMs) / 1000;
    uint32_t generated = 0;

    while (generated < totalSamples)
    {
        int blockSamples = BUFFER_SAMPLES;
        if (generated + blockSamples > totalSamples)
        {
            blockSamples = totalSamples - generated;
        }

        for (int i = 0; i < blockSamples; ++i)
        {
            float rawSample = sinf(phase) * AMPLITUDE;
            int16_t sample = (int16_t)(rawSample * volumePercent / 100.0f);

            phase += phaseStep;
            if (phase >= 2.0f * PI)
            {
                phase -= 2.0f * PI;
            }

            audioBuffer[i * 2 + 0] = leftOn  ? sample : 0;
            audioBuffer[i * 2 + 1] = rightOn ? sample : 0;
        }

        i2s_write(
            I2S_PORT,
            audioBuffer,
            blockSamples * 2 * sizeof(int16_t),
            &bytesWritten,
            portMAX_DELAY
        );

        generated += blockSamples;
    }
}

void playSilence(uint32_t durationMs)
{
    memset(audioBuffer, 0, sizeof(audioBuffer));
    size_t bytesWritten = 0;

    uint32_t totalSamples = (SAMPLE_RATE * durationMs) / 1000;
    uint32_t sent = 0;

    while (sent < totalSamples)
    {
        int blockSamples = BUFFER_SAMPLES;
        if (sent + blockSamples > totalSamples)
        {
            blockSamples = totalSamples - sent;
        }

        i2s_write(
            I2S_PORT,
            audioBuffer,
            blockSamples * 2 * sizeof(int16_t),
            &bytesWritten,
            portMAX_DELAY
        );

        sent += blockSamples;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("Init I2S...");
    setupI2S();
    Serial.println("PCM5102A stereo test");
}

void loop()
{
    Serial.println("LEFT");
    playTone(true, false, 440.0f, 1000, 50);
    playSilence(300);

    Serial.println("RIGHT");
    playTone(false, true, 440.0f, 1000, 50);
    playSilence(300);

    Serial.println("BOTH");
    playTone(true, true, 1060.0f, 500, 50);
    playSilence(600);
}