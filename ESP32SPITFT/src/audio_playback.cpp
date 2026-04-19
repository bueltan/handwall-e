#include "audio_playback.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <opus.h>
#include <string.h>

#include "pins.h"
#include "config.h"

// ============================================================
// I2S / Opus config
// ============================================================

static const uint32_t PLAYBACK_PERIOD_MS = 20;
static const size_t JITTER_SLOTS = 170;
static const int START_BUFFERED_PACKETS = 50;   // ~300 ms
static const int REBUFFER_PACKETS = 40;         // ~200 ms
static const int LOW_WATER_PACKETS = 12;
static const int MAX_CONSECUTIVE_MISSES = 10;
static const uint32_t MAX_AHEAD_WINDOW = 220;
static const uint32_t HARD_REBUFFER_GAP = 170;
static const uint32_t HARD_REBUFFER_MISSES = 24;

// ============================================================
// Packet slots
// ============================================================

struct OutputPacketSlot
{
    bool used;
    uint32_t sequence;
    uint32_t ptsSamples;
    uint16_t frameSamples;
    uint16_t payloadLen;
    uint8_t payload[OUTPUT_OPUS_MAX_PAYLOAD];
};

static OutputPacketSlot outputSlots[JITTER_SLOTS];

// ============================================================
// Decoder / playback buffers
// ============================================================

static OpusDecoder *g_opusDecoder = nullptr;
static int g_opusErr = 0;

static OutputPacketSlot g_playbackPkt;
static int16_t g_pcmOut[OUTPUT_OPUS_FRAME_SAMPLES];
static int16_t g_silenceFrame[OUTPUT_OPUS_FRAME_SAMPLES] = {0};

// ============================================================
// Shared playback state
// ============================================================

static portMUX_TYPE outputJitterMux = portMUX_INITIALIZER_UNLOCKED;

static bool outputPlaybackActive = false;
static bool outputRebuffering = true;
static bool outputEndReceived = false;
static bool outputFinished = false;
static bool outputResumeLogArmed = true;

static uint32_t expectedOutputSequence = 0;
static uint32_t highestOutputSequence = 0;
static uint32_t outputEndSequence = 0;
static uint32_t outputMissingCounter = 0;
static uint32_t lastPacketMillis = 0;

// stats
static uint32_t outputPacketsReceived = 0;
static uint32_t outputPacketsDropped = 0;
static uint32_t outputPacketsDecoded = 0;
static uint32_t outputPacketsFecRecovered = 0;
static uint32_t outputPacketsPlcRecovered = 0;
static uint32_t outputDecodeErrors = 0;
static uint32_t outputMalformedPackets = 0;
static uint32_t outputLatePackets = 0;
static uint32_t outputAheadDrops = 0;
static uint32_t outputDuplicatePackets = 0;
static uint32_t outputHardRebuffers = 0;

static uint32_t playbackUnderrunCount = 0;
static uint32_t playbackMissingSamples = 0;
static uint32_t lastUnderrunMs = 0;
static uint32_t lastPlaybackLogMs = 0;

// ============================================================
// Helpers
// ============================================================

static void clearOutputSlotsNoLock()
{
    for (size_t i = 0; i < JITTER_SLOTS; ++i)
    {
        outputSlots[i].used = false;
        outputSlots[i].sequence = 0;
        outputSlots[i].ptsSamples = 0;
        outputSlots[i].frameSamples = 0;
        outputSlots[i].payloadLen = 0;
    }
}

static int countBufferedPacketsNoLock()
{
    int count = 0;
    for (int i = 0; i < (int)JITTER_SLOTS; ++i)
    {
        if (outputSlots[i].used)
            count++;
    }
    return count;
}

static int findSlotBySeqNoLock(uint32_t seq)
{
    for (int i = 0; i < (int)JITTER_SLOTS; ++i)
    {
        if (outputSlots[i].used && outputSlots[i].sequence == seq)
            return i;
    }
    return -1;
}

static int findFreeSlotNoLock()
{
    for (int i = 0; i < (int)JITTER_SLOTS; ++i)
    {
        if (!outputSlots[i].used)
            return i;
    }
    return -1;
}

static bool getMinBufferedSeqNoLock(uint32_t &minSeq)
{
    bool found = false;
    minSeq = 0xFFFFFFFFu;

    for (int i = 0; i < (int)JITTER_SLOTS; ++i)
    {
        if (outputSlots[i].used && outputSlots[i].sequence < minSeq)
        {
            minSeq = outputSlots[i].sequence;
            found = true;
        }
    }

    return found;
}

static bool popExpectedPacketNoLock(OutputPacketSlot &out)
{
    int idx = findSlotBySeqNoLock(expectedOutputSequence);
    if (idx < 0)
        return false;

    out = outputSlots[idx];
    outputSlots[idx].used = false;
    return true;
}

static bool peekPacketBySeqNoLock(uint32_t seq, OutputPacketSlot &out)
{
    int idx = findSlotBySeqNoLock(seq);
    if (idx < 0)
        return false;

    out = outputSlots[idx];
    return true;
}

static void resetDecoderState()
{
    if (g_opusDecoder)
        opus_decoder_ctl(g_opusDecoder, OPUS_RESET_STATE);
}

static void writePcmToI2S(const int16_t *pcm, size_t samples)
{
    size_t bytesWritten = 0;
    i2s_write(
        I2S_DAC_PORT,
        pcm,
        samples * sizeof(int16_t),
        &bytesWritten,
        portMAX_DELAY);
}

static void writeSilenceFrame()
{
    writePcmToI2S(g_silenceFrame, OUTPUT_OPUS_FRAME_SAMPLES);
}

static void decodeAndWriteLossRecovery()
{
    OutputPacketSlot nextPkt;

    bool hasNext = false;
    portENTER_CRITICAL(&outputJitterMux);
    hasNext = peekPacketBySeqNoLock(expectedOutputSequence + 1, nextPkt);
    portEXIT_CRITICAL(&outputJitterMux);

    if (hasNext)
    {
        int fecSamples = opus_decode(
            g_opusDecoder,
            nextPkt.payload,
            nextPkt.payloadLen,
            g_pcmOut,
            OUTPUT_OPUS_FRAME_SAMPLES,
            1);

        if (fecSamples > 0)
        {
            writePcmToI2S(g_pcmOut, (size_t)fecSamples);
            outputPacketsFecRecovered++;
            return;
        }
    }

    int plcSamples = opus_decode(
        g_opusDecoder,
        nullptr,
        0,
        g_pcmOut,
        OUTPUT_OPUS_FRAME_SAMPLES,
        0);

    if (plcSamples > 0)
    {
        writePcmToI2S(g_pcmOut, (size_t)plcSamples);
        outputPacketsPlcRecovered++;
    }
    else
    {
        writeSilenceFrame();
        outputDecodeErrors++;
    }
}

// ============================================================
// Public API
// ============================================================

bool initOutputOpus()
{
    if (g_opusDecoder != nullptr)
        return true;

    g_opusDecoder = opus_decoder_create(
        OUTPUT_OPUS_SAMPLE_RATE,
        OUTPUT_OPUS_CHANNELS,
        &g_opusErr);

    if (!g_opusDecoder || g_opusErr != OPUS_OK)
    {
        Serial.printf("[OPUS] decoder create failed: %d\n", g_opusErr);
        g_opusDecoder = nullptr;
        return false;
    }

    Serial.println("[OPUS] output decoder ready");
    resetDecoderState();
    return true;
}

void initI2SOut()
{
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0};

    i2s_pin_config_t pins = {
        .bck_io_num = I2S_DAC_BCK,
        .ws_io_num = I2S_DAC_WS,
        .data_out_num = I2S_DAC_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE};

    i2s_driver_install(I2S_DAC_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_DAC_PORT, &pins);
    i2s_zero_dma_buffer(I2S_DAC_PORT);
}

void resetPlaybackBuffer()
{
    playbackUnderrunCount = 0;
    playbackMissingSamples = 0;
    lastUnderrunMs = 0;
    lastPlaybackLogMs = millis();
}

size_t playbackAvailableSamples()
{
    return 0;
}

void resetOutputPacketJitter()
{
    portENTER_CRITICAL(&outputJitterMux);

    clearOutputSlotsNoLock();
    outputPlaybackActive = false;
    outputRebuffering = true;
    outputEndReceived = false;
    outputFinished = false;
    outputResumeLogArmed = true;

    expectedOutputSequence = 0;
    highestOutputSequence = 0;
    outputEndSequence = 0;
    outputMissingCounter = 0;
    lastPacketMillis = 0;

    outputPacketsReceived = 0;
    outputPacketsDropped = 0;
    outputPacketsDecoded = 0;
    outputPacketsFecRecovered = 0;
    outputPacketsPlcRecovered = 0;
    outputDecodeErrors = 0;
    outputMalformedPackets = 0;
    outputLatePackets = 0;
    outputAheadDrops = 0;
    outputDuplicatePackets = 0;
    outputHardRebuffers = 0;

    portEXIT_CRITICAL(&outputJitterMux);

    resetDecoderState();
}

void markOutputAudioEnd(uint32_t endSequence)
{
    portENTER_CRITICAL(&outputJitterMux);
    outputEndReceived = true;
    outputEndSequence = endSequence;
    portEXIT_CRITICAL(&outputJitterMux);
}

bool registerOutputOpusPacket(
    uint32_t sequence,
    uint32_t ptsSamples,
    uint16_t frameSamples,
    const uint8_t *payload,
    size_t payloadLen)
{
    if (payload == NULL || payloadLen == 0 || payloadLen > OUTPUT_OPUS_MAX_PAYLOAD)
    {
        outputPacketsDropped++;
        outputMalformedPackets++;
        return false;
    }

    if (frameSamples != OUTPUT_OPUS_FRAME_SAMPLES)
    {
        outputPacketsDropped++;
        outputMalformedPackets++;
        return false;
    }

    bool inserted = false;

    portENTER_CRITICAL(&outputJitterMux);

    if (findSlotBySeqNoLock(sequence) >= 0)
    {
        outputDuplicatePackets++;
        portEXIT_CRITICAL(&outputJitterMux);
        return false;
    }
    else if (outputPlaybackActive && !outputRebuffering && sequence < expectedOutputSequence)
    {
        outputLatePackets++;
        portEXIT_CRITICAL(&outputJitterMux);
        return false;
    }
    else if (outputPlaybackActive && !outputRebuffering &&
             sequence > expectedOutputSequence + MAX_AHEAD_WINDOW)
    {
        outputPacketsDropped++;
        outputAheadDrops++;
        portEXIT_CRITICAL(&outputJitterMux);
        return false;
    }
    else
    {
        int idx = findFreeSlotNoLock();
        if (idx < 0)
        {
            outputPacketsDropped++;
            // si quieres, agrega un contador separado tipo outputBufferFullDrops++
            portEXIT_CRITICAL(&outputJitterMux);
            return false;
        }

        outputSlots[idx].used = true;
        outputSlots[idx].sequence = sequence;
        outputSlots[idx].ptsSamples = ptsSamples;
        outputSlots[idx].frameSamples = frameSamples;
        outputSlots[idx].payloadLen = (uint16_t)payloadLen;
        memcpy(outputSlots[idx].payload, payload, payloadLen);

        if (outputPacketsReceived == 0 || sequence > highestOutputSequence)
        {
            highestOutputSequence = sequence;
        }

        outputPacketsReceived++;
        lastPacketMillis = millis();
        inserted = true;
    }

    portEXIT_CRITICAL(&outputJitterMux);
    return inserted;
}

void drainOrderedOutputAudioToPlayback()
{
    // ya no se usa desde RX
}

void taskAudioPlayback(void *pvParameters)
{
    (void)pvParameters;

    TickType_t lastWake = xTaskGetTickCount();

    while (true)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(PLAYBACK_PERIOD_MS));

        bool started, rebuf, seenEnd, finished;
        uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
        int buffered = 0;
        uint32_t minSeq = 0;
        bool foundMin = false;
        uint32_t nowMs = millis();

        portENTER_CRITICAL(&outputJitterMux);
        started = outputPlaybackActive;
        rebuf = outputRebuffering;
        seenEnd = outputEndReceived;
        finished = outputFinished;
        expSeq = expectedOutputSequence;
        hiSeq = highestOutputSequence;
        eSeq = outputEndSequence;
        lastPktMs = lastPacketMillis;
        consecMiss = outputMissingCounter;
        buffered = countBufferedPacketsNoLock();
        foundMin = (buffered > 0) ? getMinBufferedSeqNoLock(minSeq) : false;
        portEXIT_CRITICAL(&outputJitterMux);

        if (finished)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (seenEnd && buffered == 0 && expSeq >= eSeq)
        {
            portENTER_CRITICAL(&outputJitterMux);
            outputPlaybackActive = false;
            outputRebuffering = true;
            outputFinished = true;
            outputMissingCounter = 0;
            portEXIT_CRITICAL(&outputJitterMux);

            Serial.println("[OUT_OPUS][END] playback completed");
            continue;
        }

        if (!seenEnd && started && buffered == 0)
        {
            if ((nowMs - lastPktMs) > 1800)
            {
                portENTER_CRITICAL(&outputJitterMux);
                outputPlaybackActive = false;
                outputRebuffering = true;
                outputFinished = true;
                outputMissingCounter = 0;
                portEXIT_CRITICAL(&outputJitterMux);

                Serial.println("[OUT_OPUS][TIMEOUT] playback completed");
                continue;
            }
        }

        if (rebuf || !started)
        {
            bool canResume = false;
            int needed = started ? REBUFFER_PACKETS : START_BUFFERED_PACKETS;

            if (seenEnd)
                canResume = (buffered > 0 && foundMin);
            else
                canResume = (buffered >= needed && foundMin);

            if (canResume)
            {
                portENTER_CRITICAL(&outputJitterMux);
                expectedOutputSequence = minSeq;
                outputPlaybackActive = true;
                outputRebuffering = false;
                outputMissingCounter = 0;
                bool shouldLog = outputResumeLogArmed;
                outputResumeLogArmed = false;
                portEXIT_CRITICAL(&outputJitterMux);

                if (shouldLog)
                {
                    Serial.printf(
                        "[PLAYBACK][START] seq=%lu buffered=%d\n",
                        (unsigned long)minSeq,
                        buffered);
                }
            }
            else
            {
                continue;
            }
        }

        bool popped = false;
        portENTER_CRITICAL(&outputJitterMux);
        popped = popExpectedPacketNoLock(g_playbackPkt);
        portEXIT_CRITICAL(&outputJitterMux);

        if (!popped)
        {
            bool bufferIsActuallyLow = (buffered <= LOW_WATER_PACKETS);
            bool tooManyMissesWithLowBuffer =
                ((consecMiss + 1) >= MAX_CONSECUTIVE_MISSES) &&
                (buffered <= (REBUFFER_PACKETS / 2));
            bool seqGapTooLarge =
                (hiSeq > expSeq) && ((hiSeq - expSeq) > HARD_REBUFFER_GAP);
            bool shouldHardRebuffer =
                seqGapTooLarge && ((consecMiss + 1) >= HARD_REBUFFER_MISSES);

            bool doRebuffer = (!seenEnd &&
                               (bufferIsActuallyLow ||
                                tooManyMissesWithLowBuffer ||
                                shouldHardRebuffer));

            portENTER_CRITICAL(&outputJitterMux);
            outputMissingCounter++;
            if (doRebuffer)
            {
                outputRebuffering = true;
                outputResumeLogArmed = true;
            }
            if (shouldHardRebuffer)
                outputHardRebuffers++;
            portEXIT_CRITICAL(&outputJitterMux);

            if (doRebuffer)
                continue;

            decodeAndWriteLossRecovery();

            portENTER_CRITICAL(&outputJitterMux);
            expectedOutputSequence++;
            portEXIT_CRITICAL(&outputJitterMux);

            playbackMissingSamples += OUTPUT_OPUS_FRAME_SAMPLES;
            playbackUnderrunCount++;
            lastUnderrunMs = millis();
            continue;
        }

        portENTER_CRITICAL(&outputJitterMux);
        expectedOutputSequence++;
        outputMissingCounter = 0;
        portEXIT_CRITICAL(&outputJitterMux);

        int decodedSamples = opus_decode(
            g_opusDecoder,
            g_playbackPkt.payload,
            g_playbackPkt.payloadLen,
            g_pcmOut,
            OUTPUT_OPUS_FRAME_SAMPLES,
            0);

        if (decodedSamples < 0)
        {
            outputDecodeErrors++;
            decodeAndWriteLossRecovery();
            playbackMissingSamples += OUTPUT_OPUS_FRAME_SAMPLES;
            playbackUnderrunCount++;
            lastUnderrunMs = millis();
            continue;
        }

        writePcmToI2S(g_pcmOut, (size_t)decodedSamples);
        outputPacketsDecoded++;

        if (millis() - lastPlaybackLogMs >= 2000)
        {
            lastPlaybackLogMs = millis();

            int bufferedNow = 0;
            portENTER_CRITICAL(&outputJitterMux);
            bufferedNow = countBufferedPacketsNoLock();
            portEXIT_CRITICAL(&outputJitterMux);

            Serial.printf(
                "[PLAYBACK] started=%d | buffered=%d/%d | underruns=%lu | missing=%lu | fec=%lu | plc=%lu | dec=%lu | drop=%lu | decErr=%lu | resync=%lu | last_underrun_ms=%lu\n",
                outputPlaybackActive ? 1 : 0,
                bufferedNow,
                (int)JITTER_SLOTS,
                (unsigned long)playbackUnderrunCount,
                (unsigned long)playbackMissingSamples,
                (unsigned long)outputPacketsFecRecovered,
                (unsigned long)outputPacketsPlcRecovered,
                (unsigned long)outputPacketsDecoded,
                (unsigned long)outputPacketsDropped,
                (unsigned long)outputDecodeErrors,
                (unsigned long)outputHardRebuffers,
                (unsigned long)lastUnderrunMs);
        }
    }
}