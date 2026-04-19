#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

constexpr uint8_t OPUS_MAGIC_BYTES[4] = {'O', 'P', 'U', 'S'};
constexpr uint8_t OPUS_VERSION = 1;
constexpr uint8_t OPUS_FLAG_END = 0x01;

constexpr uint32_t OUTPUT_OPUS_SAMPLE_RATE = 16000;
constexpr uint8_t OUTPUT_OPUS_CHANNELS = 1;
constexpr uint16_t OUTPUT_OPUS_FRAME_MS = 20;
constexpr uint16_t OUTPUT_OPUS_FRAME_SAMPLES = 320;   // 20 ms @ 16 kHz mono
constexpr size_t OUTPUT_PCM_FRAME_BYTES = OUTPUT_OPUS_FRAME_SAMPLES * 2;
constexpr size_t OUTPUT_OPUS_MAX_PAYLOAD = 400;

struct OpusUdpHeader
{
    uint8_t magic[4];
    uint8_t version;
    uint8_t flags;
    uint32_t sequence_be;
    uint32_t pts_samples_be;
    uint16_t frame_samples_be;
    uint16_t payload_len_be;
} __attribute__((packed));

void initI2SOut();
bool initOutputOpus();
void resetPlaybackBuffer();
size_t playbackAvailableSamples();
void taskAudioPlayback(void *pvParameters);

// Output jitter / decode API
void resetOutputPacketJitter();
void markOutputAudioEnd(uint32_t endSequence);
bool registerOutputOpusPacket(
    uint32_t sequence,
    uint32_t ptsSamples,
    uint16_t frameSamples,
    const uint8_t *payload,
    size_t payloadLen
);
void drainOrderedOutputAudioToPlayback();