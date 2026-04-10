#include "audio_playback.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <string.h>

#include "pins.h"
#include "config.h"

static const size_t I2S_BLOCK_SAMPLES = 256;
static const size_t PREBUFFER_SAMPLES = 3200;   // ~100 ms at 16 kHz
static const size_t FIFO_SAMPLES      = 16384;   // about 512 ms mono

static int16_t audioFifo[FIFO_SAMPLES];
static int16_t stereoOutBuffer[I2S_BLOCK_SAMPLES * 2];

static volatile size_t fifoWriteIndex = 0;
static volatile size_t fifoReadIndex  = 0;
static volatile size_t fifoCount      = 0;

static volatile bool playbackStarted = false;

static portMUX_TYPE playbackMux = portMUX_INITIALIZER_UNLOCKED;

static void fifoPushSample(int16_t sample)
{
    portENTER_CRITICAL(&playbackMux);

    if (fifoCount < FIFO_SAMPLES)
    {
        audioFifo[fifoWriteIndex] = sample;
        fifoWriteIndex = (fifoWriteIndex + 1) % FIFO_SAMPLES;
        fifoCount++;
    }

    portEXIT_CRITICAL(&playbackMux);
}

static bool fifoPopSample(int16_t& sample)
{
    bool ok = false;

    portENTER_CRITICAL(&playbackMux);

    if (fifoCount > 0)
    {
        sample = audioFifo[fifoReadIndex];
        fifoReadIndex = (fifoReadIndex + 1) % FIFO_SAMPLES;
        fifoCount--;
        ok = true;
    }

    portEXIT_CRITICAL(&playbackMux);
    return ok;
}

size_t playbackAvailableSamples()
{
    size_t count = 0;

    portENTER_CRITICAL(&playbackMux);
    count = fifoCount;
    portEXIT_CRITICAL(&playbackMux);

    return count;
}

void resetPlaybackBuffer()
{
    portENTER_CRITICAL(&playbackMux);
    fifoWriteIndex = 0;
    fifoReadIndex = 0;
    fifoCount = 0;
    playbackStarted = false;
    portEXIT_CRITICAL(&playbackMux);
}

void initI2SOut()
{
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_DAC_BCK,
        .ws_io_num = I2S_DAC_WS,
        .data_out_num = I2S_DAC_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_DAC_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_DAC_PORT, &pins);
    i2s_zero_dma_buffer(I2S_DAC_PORT);
}

void pushUdpAudioToPlayback(const uint8_t* data, size_t lenBytes)
{
    if (data == NULL || lenBytes < 2)
        return;

    size_t monoSamples = lenBytes / 2;

    for (size_t i = 0; i < monoSamples; ++i)
    {
        size_t byteIndex = i * 2;

        int16_t sample = (int16_t)(
            ((uint16_t)data[byteIndex]) |
            ((uint16_t)data[byteIndex + 1] << 8)
        );

        fifoPushSample(sample);
    }
}

static void fillStereoSilence(int16_t* outBuffer, size_t monoSamples)
{
    memset(outBuffer, 0, monoSamples * 2 * sizeof(int16_t));
}

static size_t fillStereoFromFifo(int16_t* outBuffer, size_t monoSamples)
{
    size_t actualSamples = 0;

    for (size_t i = 0; i < monoSamples; ++i)
    {
        int16_t sample = 0;
        if (!fifoPopSample(sample))
            break;

        outBuffer[i * 2 + 0] = sample;
        outBuffer[i * 2 + 1] = sample;
        actualSamples++;
    }

    for (size_t i = actualSamples; i < monoSamples; ++i)
    {
        outBuffer[i * 2 + 0] = 0;
        outBuffer[i * 2 + 1] = 0;
    }

    return actualSamples;
}

static void writeStereoBlockToI2S(int16_t* stereoData, size_t monoSamples)
{
    size_t bytesWritten = 0;

    i2s_write(
        I2S_DAC_PORT,
        stereoData,
        monoSamples * 2 * sizeof(int16_t),
        &bytesWritten,
        portMAX_DELAY
    );
}

void taskAudioPlayback(void* pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        size_t available = playbackAvailableSamples();

        if (!playbackStarted)
        {
            if (available >= PREBUFFER_SAMPLES)
            {
                playbackStarted = true;
            }
            else
            {
                fillStereoSilence(stereoOutBuffer, I2S_BLOCK_SAMPLES);
                writeStereoBlockToI2S(stereoOutBuffer, I2S_BLOCK_SAMPLES);
                continue;
            }
        }

        size_t actualSamples = fillStereoFromFifo(stereoOutBuffer, I2S_BLOCK_SAMPLES);
        writeStereoBlockToI2S(stereoOutBuffer, I2S_BLOCK_SAMPLES);

        if (actualSamples == 0)
        {
            playbackStarted = false;
        }
    }
}