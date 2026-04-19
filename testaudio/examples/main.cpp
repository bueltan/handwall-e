// #include <Arduino.h>
// #include <WiFi.h>
// #include <WiFiUdp.h>
// #include "driver/i2s.h"
// #include <opus.h>

// // ==============================
// // WiFi / UDP
// // ==============================
// static const char *WIFI_SSID = "G";
// static const char *WIFI_PASS = "Abc123x1";

// static const char *SERVER_IP = "74.208.158.226";
// static const uint16_t SERVER_PORT = 9999;
// static const uint16_t LOCAL_UDP_PORT = 12000;

// WiFiUDP udp;

// // ==============================
// // Audio / Opus config
// // ==============================
// static const uint32_t SAMPLE_RATE = 16000;
// static const uint8_t CHANNELS = 1;
// static const uint16_t FRAME_SAMPLES = 320; // 20 ms @ 16 kHz
// static const uint16_t MAX_DECODE_SAMPLES = FRAME_SAMPLES;

// // ==============================
// // I2S pins
// // ==============================
// static const int I2S_BCK_PIN = 4;
// static const int I2S_WS_PIN = 5;
// static const int I2S_DATA_PIN = 7;
// static const i2s_port_t I2S_PORT = I2S_NUM_0;

// // ==============================
// // Protocol
// // ==============================
// static const uint8_t MAGIC[4] = {'O', 'P', 'U', 'S'};
// static const uint8_t VERSION = 1;
// static const uint8_t FLAG_END = 0x01;
// static const size_t HEADER_SIZE = 18;

// // ==============================
// // Jitter / reorder buffer
// // ==============================
// static const int JITTER_SLOTS = 120;
// static const int START_BUFFERED_PACKETS = 40; // ~800 ms if 20 ms packets
// static const int REBUFFER_PACKETS = 30;       // ~600 ms
// static const int LOW_WATER_PACKETS = 10;
// static const int MAX_CONSECUTIVE_MISSES = 10;
// static const int MAX_OPUS_PAYLOAD = 400;

// // ==============================
// // Timing
// // ==============================
// static const uint32_t START_RETRY_MS = 3000;
// static const uint32_t END_TIMEOUT_MS = 1500;
// static const uint32_t STATS_PERIOD_MS = 2000;

// // ==============================
// // Packet slot
// // ==============================
// struct PacketSlot
// {
//     bool used;
//     uint32_t seq;
//     uint32_t pts_samples;
//     uint16_t frame_samples;
//     uint16_t payload_len;
//     uint8_t flags;
//     uint8_t payload[MAX_OPUS_PAYLOAD];
// };

// static PacketSlot jitter[JITTER_SLOTS];

// // ==============================
// // Shared state
// // ==============================
// static volatile bool playbackStarted = false;
// static volatile bool rebuffering = true;
// static volatile bool endSeen = false;
// static volatile bool streamFinished = false;

// static volatile uint32_t expectedSeq = 0;
// static volatile uint32_t highestSeqSeen = 0;
// static volatile uint32_t endSeq = 0;
// static volatile uint32_t consecutiveMisses = 0;
// static volatile uint32_t lastPacketMillis = 0;
// static volatile uint32_t startSentMillis = 0;

// // stats
// static volatile uint32_t receivedPackets = 0;
// static volatile uint32_t droppedPackets = 0;
// static volatile uint32_t duplicatePackets = 0;
// static volatile uint32_t latePackets = 0;
// static volatile uint32_t missingPackets = 0;
// static volatile uint32_t decodedPackets = 0;
// static volatile uint32_t decodeErrors = 0;
// static volatile uint32_t rejectedPackets = 0;

// // decoder and playback buffers
// static OpusDecoder *opusDecoder = nullptr;
// static int opusErr = 0;

// static PacketSlot playbackPkt;
// static int16_t pcmOut[MAX_DECODE_SAMPLES];
// static int16_t silenceFrame[FRAME_SAMPLES] = {0};

// // RTOS
// static SemaphoreHandle_t jitterMutex = nullptr;
// static TaskHandle_t udpRxTaskHandle = nullptr;
// static TaskHandle_t playbackTaskHandle = nullptr;
// static TaskHandle_t controlTaskHandle = nullptr;
// static TaskHandle_t statsTaskHandle = nullptr;

// // ==============================
// // Helpers
// // ==============================
// static uint16_t read_be16(const uint8_t *p)
// {
//     return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
// }

// static uint32_t read_be32(const uint8_t *p)
// {
//     return (uint32_t(p[0]) << 24) |
//            (uint32_t(p[1]) << 16) |
//            (uint32_t(p[2]) << 8) |
//            uint32_t(p[3]);
// }

// static void clearJitterNoLock()
// {
//     for (int i = 0; i < JITTER_SLOTS; ++i)
//     {
//         jitter[i].used = false;
//         jitter[i].seq = 0;
//         jitter[i].pts_samples = 0;
//         jitter[i].frame_samples = 0;
//         jitter[i].payload_len = 0;
//         jitter[i].flags = 0;
//     }
// }

// static int countBufferedPacketsNoLock()
// {
//     int count = 0;
//     for (int i = 0; i < JITTER_SLOTS; ++i)
//     {
//         if (jitter[i].used)
//         {
//             count++;
//         }
//     }
//     return count;
// }

// static int findSlotBySeqNoLock(uint32_t seq)
// {
//     for (int i = 0; i < JITTER_SLOTS; ++i)
//     {
//         if (jitter[i].used && jitter[i].seq == seq)
//         {
//             return i;
//         }
//     }
//     return -1;
// }

// static int findFreeSlotNoLock()
// {
//     for (int i = 0; i < JITTER_SLOTS; ++i)
//     {
//         if (!jitter[i].used)
//         {
//             return i;
//         }
//     }
//     return -1;
// }

// static bool getMinBufferedSeqNoLock(uint32_t &minSeq)
// {
//     bool found = false;
//     minSeq = 0xFFFFFFFFu;

//     for (int i = 0; i < JITTER_SLOTS; ++i)
//     {
//         if (jitter[i].used && jitter[i].seq < minSeq)
//         {
//             minSeq = jitter[i].seq;
//             found = true;
//         }
//     }

//     return found;
// }

// static bool getStateSnapshot(bool &started, bool &rebuf, bool &seenEnd, bool &finished,
//                              uint32_t &expSeq, uint32_t &hiSeq, uint32_t &eSeq,
//                              uint32_t &lastPktMs, uint32_t &consecMiss)
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) != pdTRUE)
//     {
//         return false;
//     }

//     started = playbackStarted;
//     rebuf = rebuffering;
//     seenEnd = endSeen;
//     finished = streamFinished;
//     expSeq = expectedSeq;
//     hiSeq = highestSeqSeen;
//     eSeq = endSeq;
//     lastPktMs = lastPacketMillis;
//     consecMiss = consecutiveMisses;

//     xSemaphoreGive(jitterMutex);
//     return true;
// }

// static void resetStreamState()
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         clearJitterNoLock();

//         playbackStarted = false;
//         rebuffering = true;
//         endSeen = false;
//         streamFinished = false;

//         expectedSeq = 0;
//         highestSeqSeen = 0;
//         endSeq = 0;
//         consecutiveMisses = 0;
//         lastPacketMillis = 0;

//         receivedPackets = 0;
//         droppedPackets = 0;
//         duplicatePackets = 0;
//         latePackets = 0;
//         missingPackets = 0;
//         decodedPackets = 0;
//         decodeErrors = 0;
//         rejectedPackets = 0;

//         xSemaphoreGive(jitterMutex);
//     }
// }

// static int getBufferedCountLocked()
// {
//     int buffered = 0;
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         buffered = countBufferedPacketsNoLock();
//         xSemaphoreGive(jitterMutex);
//     }
//     return buffered;
// }

// static bool getMinBufferedSeqLocked(uint32_t &minSeq)
// {
//     bool found = false;
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         found = getMinBufferedSeqNoLock(minSeq);
//         xSemaphoreGive(jitterMutex);
//     }
//     return found;
// }

// static bool popExpectedPacketLocked(PacketSlot &out)
// {
//     bool ok = false;

//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         int idx = findSlotBySeqNoLock(expectedSeq);
//         if (idx >= 0)
//         {
//             out = jitter[idx];
//             jitter[idx].used = false;
//             ok = true;
//         }
//         xSemaphoreGive(jitterMutex);
//     }

//     return ok;
// }

// static bool insertPacketLocked(uint32_t seq,
//                                uint32_t pts_samples,
//                                uint16_t frame_samples,
//                                uint16_t payload_len,
//                                uint8_t flags,
//                                const uint8_t *payload)
// {
//     bool inserted = false;

//     if (payload_len > MAX_OPUS_PAYLOAD)
//     {
//         if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//         {
//             droppedPackets++;
//             xSemaphoreGive(jitterMutex);
//         }
//         return false;
//     }

//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         if (findSlotBySeqNoLock(seq) >= 0)
//         {
//             duplicatePackets++;
//         }
//         else if (playbackStarted && !rebuffering && seq < expectedSeq)
//         {
//             latePackets++;
//         }
//         else
//         {
//             int idx = findFreeSlotNoLock();
//             if (idx < 0)
//             {
//                 droppedPackets++;
//             }
//             else
//             {
//                 jitter[idx].used = true;
//                 jitter[idx].seq = seq;
//                 jitter[idx].pts_samples = pts_samples;
//                 jitter[idx].frame_samples = frame_samples;
//                 jitter[idx].payload_len = payload_len;
//                 jitter[idx].flags = flags;
//                 memcpy(jitter[idx].payload, payload, payload_len);

//                 if (seq > highestSeqSeen)
//                 {
//                     highestSeqSeen = seq;
//                 }

//                 receivedPackets++;
//                 lastPacketMillis = millis();
//                 inserted = true;
//             }
//         }

//         xSemaphoreGive(jitterMutex);
//     }

//     return inserted;
// }

// static void markEndPacket(uint32_t seq)
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         endSeen = true;
//         endSeq = seq;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void setPlaybackState(bool started, bool rebuf, bool finished, uint32_t consecMiss)
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         playbackStarted = started;
//         rebuffering = rebuf;
//         streamFinished = finished;
//         consecutiveMisses = consecMiss;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void setExpectedSeqAndResume(uint32_t seq)
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         expectedSeq = seq;
//         playbackStarted = true;
//         rebuffering = false;
//         consecutiveMisses = 0;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void advanceExpectedSeq()
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         expectedSeq++;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void incrementMissingAndMaybeRebuffer(bool doRebuffer)
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         missingPackets++;
//         consecutiveMisses++;
//         if (doRebuffer)
//         {
//             rebuffering = true;
//         }
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void resetConsecutiveMisses()
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         consecutiveMisses = 0;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void incrementDecodeErrors()
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         decodeErrors++;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void incrementDecodedPackets()
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         decodedPackets++;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// static void incrementRejectedPackets()
// {
//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         rejectedPackets++;
//         xSemaphoreGive(jitterMutex);
//     }
// }

// // ==============================
// // Init
// // ==============================
// static bool i2sInit()
// {
//     i2s_config_t config = {};
//     config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
//     config.sample_rate = SAMPLE_RATE;
//     config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
//     config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
//     config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
//     config.intr_alloc_flags = 0;
//     config.dma_buf_count = 8;
//     config.dma_buf_len = 256;
//     config.use_apll = false;
//     config.tx_desc_auto_clear = true;
//     config.fixed_mclk = 0;

//     i2s_pin_config_t pins = {};
//     pins.bck_io_num = I2S_BCK_PIN;
//     pins.ws_io_num = I2S_WS_PIN;
//     pins.data_out_num = I2S_DATA_PIN;
//     pins.data_in_num = I2S_PIN_NO_CHANGE;

//     esp_err_t err = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
//     if (err != ESP_OK)
//     {
//         Serial.printf("[I2S] driver_install failed: %d\n", err);
//         return false;
//     }

//     err = i2s_set_pin(I2S_PORT, &pins);
//     if (err != ESP_OK)
//     {
//         Serial.printf("[I2S] set_pin failed: %d\n", err);
//         return false;
//     }

//     err = i2s_zero_dma_buffer(I2S_PORT);
//     if (err != ESP_OK)
//     {
//         Serial.printf("[I2S] zero_dma_buffer failed: %d\n", err);
//         return false;
//     }

//     return true;
// }

// static bool opusInit()
// {
//     opusDecoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &opusErr);
//     if (!opusDecoder || opusErr != OPUS_OK)
//     {
//         Serial.printf("[OPUS] decoder create failed: %d\n", opusErr);
//         return false;
//     }

//     return true;
// }

// // ==============================
// // UDP
// // ==============================
// static void sendStart()
// {
//     resetStreamState();

//     if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
//     {
//         startSentMillis = millis();
//         xSemaphoreGive(jitterMutex);
//     }

//     udp.beginPacket(SERVER_IP, SERVER_PORT);
//     udp.write((const uint8_t *)"START", 5);
//     udp.endPacket();

//     Serial.println("[UDP] START sent");
// }

// static bool parseAndQueuePacket(const uint8_t *buf, size_t len)
// {
//     if (len < HEADER_SIZE)
//     {
//         return false;
//     }

//     if (memcmp(buf, MAGIC, 4) != 0)
//     {
//         return false;
//     }

//     uint8_t version = buf[4];
//     uint8_t flags = buf[5];
//     uint32_t seq = read_be32(buf + 6);
//     uint32_t pts_samples = read_be32(buf + 10);
//     uint16_t frame_samples = read_be16(buf + 14);
//     uint16_t payload_len = read_be16(buf + 16);

//     if (version != VERSION)
//     {
//         return false;
//     }

//     if ((HEADER_SIZE + payload_len) != len)
//     {
//         return false;
//     }

//     if ((flags & FLAG_END) != 0)
//     {
//         markEndPacket(seq);
//         Serial.printf("[UDP] END packet seq=%lu\n", (unsigned long)seq);
//         return true;
//     }

//     if (frame_samples != FRAME_SAMPLES)
//     {
//         return false;
//     }

//     return insertPacketLocked(seq, pts_samples, frame_samples, payload_len, flags, buf + HEADER_SIZE);
// }

// // ==============================
// // Audio output
// // ==============================
// static void writePcmToI2S(const int16_t *pcm, size_t samples)
// {
//     size_t bytesWritten = 0;
//     i2s_write(I2S_PORT, pcm, samples * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
// }

// static void writeSilenceFrame()
// {
//     writePcmToI2S(silenceFrame, FRAME_SAMPLES);
// }

// // ==============================
// // Tasks
// // ==============================
// static void udpRxTask(void *parameter)
// {
//     static uint8_t rxBuf[HEADER_SIZE + MAX_OPUS_PAYLOAD + 16];

//     while (true)
//     {
//         int packetSize = udp.parsePacket();
//         if (packetSize > 0)
//         {
//             int n = udp.read(rxBuf, sizeof(rxBuf));
//             if (n > 0)
//             {
//                 bool ok = parseAndQueuePacket(rxBuf, (size_t)n);
//                 if (!ok)
//                 {
//                     incrementRejectedPackets();
//                 }
//             }
//         }
//         else
//         {
//             vTaskDelay(pdMS_TO_TICKS(1));
//         }
//     }
// }

// static void playbackTask(void *parameter)
// {
//     TickType_t lastWake = xTaskGetTickCount();

//     while (true)
//     {
//         vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(20));

//         bool started, rebuf, seenEnd, finished;
//         uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
//         getStateSnapshot(started, rebuf, seenEnd, finished, expSeq, hiSeq, eSeq, lastPktMs, consecMiss);

//         int buffered = getBufferedCountLocked();
//         uint32_t minSeq = 0;
//         bool foundMin = (buffered > 0) ? getMinBufferedSeqLocked(minSeq) : false;
//         uint32_t nowMs = millis();

//         if (finished)
//         {
//             vTaskDelay(pdMS_TO_TICKS(100));
//             continue;
//         }

//         if (seenEnd && buffered == 0 && expSeq >= eSeq)
//         {
//             setPlaybackState(false, true, true, 0);
//             Serial.println("[PLAYBACK] stream finished by END");
//             continue;
//         }

//         if (!seenEnd && buffered == 0 && started)
//         {
//             if ((nowMs - lastPktMs) > END_TIMEOUT_MS)
//             {
//                 setPlaybackState(false, true, true, 0);
//                 Serial.println("[PLAYBACK] stream finished by timeout");
//                 continue;
//             }
//         }

//         if (rebuf || !started)
//         {
//             bool canResume = false;

//             if (seenEnd)
//             {
//                 canResume = (buffered > 0 && foundMin);
//             }
//             else
//             {
//                 int needed = started ? REBUFFER_PACKETS : START_BUFFERED_PACKETS;
//                 canResume = (buffered >= needed && foundMin);
//             }

//             if (canResume)
//             {
//                 setExpectedSeqAndResume(minSeq);
//                 Serial.printf("[PLAYBACK] start/resume at seq=%lu buffered=%d\n",
//                               (unsigned long)minSeq, buffered);
//             }
//             else
//             {
//                 continue;
//             }
//         }

//         if (!popExpectedPacketLocked(playbackPkt))
//         {
//             bool doRebuffer = (!seenEnd && (buffered <= LOW_WATER_PACKETS || consecMiss + 1 >= MAX_CONSECUTIVE_MISSES));
//             incrementMissingAndMaybeRebuffer(doRebuffer);

//             if (seenEnd)
//             {
//                 writeSilenceFrame();
//                 advanceExpectedSeq();
//                 continue;
//             }

//             if (doRebuffer)
//             {
//                 continue;
//             }

//             writeSilenceFrame();
//             advanceExpectedSeq();
//             continue;
//         }

//         resetConsecutiveMisses();

//         int decodedSamples = opus_decode(
//             opusDecoder,
//             playbackPkt.payload,
//             playbackPkt.payload_len,
//             pcmOut,
//             FRAME_SAMPLES,
//             0);

//         if (decodedSamples < 0)
//         {
//             incrementDecodeErrors();
//             writeSilenceFrame();
//             advanceExpectedSeq();
//             continue;
//         }

//         writePcmToI2S(pcmOut, decodedSamples);
//         incrementDecodedPackets();
//         advanceExpectedSeq();
//     }
// }

// static void controlTask(void *parameter)
// {
//     while (true)
//     {
//         bool started, rebuf, seenEnd, finished;
//         uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
//         getStateSnapshot(started, rebuf, seenEnd, finished, expSeq, hiSeq, eSeq, lastPktMs, consecMiss);

//         uint32_t now = millis();
//         bool noActivityYet = (!started && !finished && !seenEnd && (receivedPackets == 0));

//         if (noActivityYet)
//         {
//             uint32_t elapsed = now - startSentMillis;
//             if (elapsed >= START_RETRY_MS)
//             {
//                 Serial.println("[CTRL] retry START");
//                 sendStart();
//             }
//         }

//         vTaskDelay(pdMS_TO_TICKS(100));
//     }
// }

// static void statsTask(void *parameter)
// {
//     while (true)
//     {
//         int buffered = getBufferedCountLocked();

//         bool started, rebuf, seenEnd, finished;
//         uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
//         getStateSnapshot(started, rebuf, seenEnd, finished, expSeq, hiSeq, eSeq, lastPktMs, consecMiss);

//         Serial.printf(
//             "[STATS] rx=%lu dec=%lu miss=%lu drop=%lu dup=%lu late=%lu rej=%lu decErr=%lu buffered=%d expected=%lu highest=%lu end=%d rebuf=%d finished=%d\n",
//             (unsigned long)receivedPackets,
//             (unsigned long)decodedPackets,
//             (unsigned long)missingPackets,
//             (unsigned long)droppedPackets,
//             (unsigned long)duplicatePackets,
//             (unsigned long)latePackets,
//             (unsigned long)rejectedPackets,
//             (unsigned long)decodeErrors,
//             buffered,
//             (unsigned long)expSeq,
//             (unsigned long)hiSeq,
//             seenEnd ? 1 : 0,
//             rebuf ? 1 : 0,
//             finished ? 1 : 0);

//         vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));
//     }
// }

// // ==============================
// // Arduino
// // ==============================
// void setup()
// {
//     Serial.begin(115200);
//     delay(1000);

//     jitterMutex = xSemaphoreCreateMutex();
//     if (jitterMutex == nullptr)
//     {
//         Serial.println("[RTOS] failed to create jitter mutex");
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     resetStreamState();

//     WiFi.mode(WIFI_STA);
//     WiFi.begin(WIFI_SSID, WIFI_PASS);

//     Serial.print("[WIFI] connecting");
//     while (WiFi.status() != WL_CONNECTED)
//     {
//         Serial.print(".");
//         delay(500);
//     }

//     WiFi.setSleep(false);
//     Serial.println();
//     Serial.print("[WIFI] IP: ");
//     Serial.println(WiFi.localIP());

//     if (!udp.begin(LOCAL_UDP_PORT))
//     {
//         Serial.println("[UDP] begin failed");
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     if (!i2sInit())
//     {
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     if (!opusInit())
//     {
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     if (xTaskCreate(udpRxTask, "udpRxTask", 4096, nullptr, 3, &udpRxTaskHandle) != pdPASS)
//     {
//         Serial.println("[RTOS] failed to create udpRxTask");
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     if (xTaskCreate(playbackTask, "playbackTask", 12288, nullptr, 2, &playbackTaskHandle) != pdPASS)
//     {
//         Serial.println("[RTOS] failed to create playbackTask");
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     if (xTaskCreate(controlTask, "controlTask", 4096, nullptr, 2, &controlTaskHandle) != pdPASS)
//     {
//         Serial.println("[RTOS] failed to create controlTask");
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     if (xTaskCreate(statsTask, "statsTask", 4096, nullptr, 1, &statsTaskHandle) != pdPASS)
//     {
//         Serial.println("[RTOS] failed to create statsTask");
//         while (true)
//         {
//             delay(1000);
//         }
//     }

//     sendStart();
// }

// void loop()
// {
//     vTaskDelay(portMAX_DELAY);
// }


#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "driver/i2s.h"
#include <opus.h>

// ==============================
// WiFi / UDP
// ==============================
static const char *WIFI_SSID = "G";
static const char *WIFI_PASS = "Abc123x1";

static const char *SERVER_IP = "74.208.158.226";
static const uint16_t SERVER_PORT = 9999;
static const uint16_t LOCAL_UDP_PORT = 12000;

WiFiUDP udp;

// ==============================
// Audio / Opus config
// ==============================
static const uint32_t SAMPLE_RATE = 16000;
static const uint8_t CHANNELS = 1;
static const uint16_t FRAME_SAMPLES = 320; // 20 ms @ 16 kHz
static const uint16_t MAX_DECODE_SAMPLES = FRAME_SAMPLES;

// ==============================
// I2S pins
// ==============================
static const int I2S_BCK_PIN = 4;
static const int I2S_WS_PIN = 5;
static const int I2S_DATA_PIN = 7;
static const i2s_port_t I2S_PORT = I2S_NUM_0;

// ==============================
// Protocol
// ==============================
static const uint8_t MAGIC[4] = {'O', 'P', 'U', 'S'};
static const uint8_t VERSION = 1;
static const uint8_t FLAG_END = 0x01;
static const size_t HEADER_SIZE = 18;

// ==============================
// Jitter / reorder buffer
// ==============================
static const int JITTER_SLOTS = 220;
static const int START_BUFFERED_PACKETS = 50; // 1.0 s
static const int REBUFFER_PACKETS = 40;       // 0.8 s
static const int LOW_WATER_PACKETS = 12;
static const int MAX_CONSECUTIVE_MISSES = 10; // less aggressive than before
static const int MAX_OPUS_PAYLOAD = 400;

// ==============================
// Timing
// ==============================
static const uint32_t START_RETRY_MS = 3000;
static const uint32_t END_TIMEOUT_MS = 1800;
static const uint32_t STATS_PERIOD_MS = 2000;
static const uint32_t PLAYBACK_PERIOD_MS = 20;

// ==============================
// Packet slot
// ==============================
struct PacketSlot
{
    bool used;
    uint32_t seq;
    uint32_t pts_samples;
    uint16_t frame_samples;
    uint16_t payload_len;
    uint8_t flags;
    uint8_t payload[MAX_OPUS_PAYLOAD];
};

static PacketSlot jitter[JITTER_SLOTS];

// ==============================
// Shared state
// ==============================
static volatile bool playbackStarted = false;
static volatile bool rebuffering = true;
static volatile bool endSeen = false;
static volatile bool streamFinished = false;

// helps avoid noisy repeated logs
static volatile bool resumeLogArmed = true;

static volatile uint32_t expectedSeq = 0;
static volatile uint32_t highestSeqSeen = 0;
static volatile uint32_t endSeq = 0;
static volatile uint32_t consecutiveMisses = 0;
static volatile uint32_t lastPacketMillis = 0;
static volatile uint32_t startSentMillis = 0;

// stats
static volatile uint32_t receivedPackets = 0;
static volatile uint32_t droppedPackets = 0;              // general total drops
static volatile uint32_t bufferFullDrops = 0;             // drop due to full jitter buffer
static volatile uint32_t duplicatePackets = 0;
static volatile uint32_t latePackets = 0;
static volatile uint32_t missingPackets = 0;
static volatile uint32_t decodedPackets = 0;
static volatile uint32_t decodeErrors = 0;
static volatile uint32_t malformedPackets = 0;            // invalid header/size/version/etc
static volatile uint32_t plcPackets = 0;
static volatile uint32_t evictedOldPackets = 0;           // old slot replaced by newer packet

// decoder and playback buffers
static OpusDecoder *opusDecoder = nullptr;
static int opusErr = 0;

static PacketSlot playbackPkt;
static int16_t pcmOut[MAX_DECODE_SAMPLES];
static int16_t silenceFrame[FRAME_SAMPLES] = {0};

// RTOS
static SemaphoreHandle_t jitterMutex = nullptr;
static TaskHandle_t udpRxTaskHandle = nullptr;
static TaskHandle_t playbackTaskHandle = nullptr;
static TaskHandle_t controlTaskHandle = nullptr;
static TaskHandle_t statsTaskHandle = nullptr;

// ==============================
// Helpers
// ==============================
static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) |
           (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}

static void clearJitterNoLock()
{
    for (int i = 0; i < JITTER_SLOTS; ++i)
    {
        jitter[i].used = false;
        jitter[i].seq = 0;
        jitter[i].pts_samples = 0;
        jitter[i].frame_samples = 0;
        jitter[i].payload_len = 0;
        jitter[i].flags = 0;
    }
}

static int countBufferedPacketsNoLock()
{
    int count = 0;
    for (int i = 0; i < JITTER_SLOTS; ++i)
    {
        if (jitter[i].used)
        {
            count++;
        }
    }
    return count;
}

static int findSlotBySeqNoLock(uint32_t seq)
{
    for (int i = 0; i < JITTER_SLOTS; ++i)
    {
        if (jitter[i].used && jitter[i].seq == seq)
        {
            return i;
        }
    }
    return -1;
}

static int findFreeSlotNoLock()
{
    for (int i = 0; i < JITTER_SLOTS; ++i)
    {
        if (!jitter[i].used)
        {
            return i;
        }
    }
    return -1;
}

static int findOldestSlotNoLock()
{
    int idx = -1;
    uint32_t minSeq = 0xFFFFFFFFu;

    for (int i = 0; i < JITTER_SLOTS; ++i)
    {
        if (jitter[i].used && jitter[i].seq < minSeq)
        {
            minSeq = jitter[i].seq;
            idx = i;
        }
    }

    return idx;
}

static bool getMinBufferedSeqNoLock(uint32_t &minSeq)
{
    bool found = false;
    minSeq = 0xFFFFFFFFu;

    for (int i = 0; i < JITTER_SLOTS; ++i)
    {
        if (jitter[i].used && jitter[i].seq < minSeq)
        {
            minSeq = jitter[i].seq;
            found = true;
        }
    }

    return found;
}

static bool getStateSnapshot(bool &started, bool &rebuf, bool &seenEnd, bool &finished,
                             uint32_t &expSeq, uint32_t &hiSeq, uint32_t &eSeq,
                             uint32_t &lastPktMs, uint32_t &consecMiss)
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    started = playbackStarted;
    rebuf = rebuffering;
    seenEnd = endSeen;
    finished = streamFinished;
    expSeq = expectedSeq;
    hiSeq = highestSeqSeen;
    eSeq = endSeq;
    lastPktMs = lastPacketMillis;
    consecMiss = consecutiveMisses;

    xSemaphoreGive(jitterMutex);
    return true;
}

static void resetDecoderState()
{
    if (opusDecoder)
    {
        opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
    }
}

static void resetStreamState()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        clearJitterNoLock();

        playbackStarted = false;
        rebuffering = true;
        endSeen = false;
        streamFinished = false;
        resumeLogArmed = true;

        expectedSeq = 0;
        highestSeqSeen = 0;
        endSeq = 0;
        consecutiveMisses = 0;
        lastPacketMillis = 0;

        receivedPackets = 0;
        droppedPackets = 0;
        bufferFullDrops = 0;
        duplicatePackets = 0;
        latePackets = 0;
        missingPackets = 0;
        decodedPackets = 0;
        decodeErrors = 0;
        malformedPackets = 0;
        plcPackets = 0;
        evictedOldPackets = 0;

        xSemaphoreGive(jitterMutex);
    }

    resetDecoderState();
}

static int getBufferedCountLocked()
{
    int buffered = 0;
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        buffered = countBufferedPacketsNoLock();
        xSemaphoreGive(jitterMutex);
    }
    return buffered;
}

static bool getMinBufferedSeqLocked(uint32_t &minSeq)
{
    bool found = false;
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        found = getMinBufferedSeqNoLock(minSeq);
        xSemaphoreGive(jitterMutex);
    }
    return found;
}

static bool popExpectedPacketLocked(PacketSlot &out)
{
    bool ok = false;

    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        int idx = findSlotBySeqNoLock(expectedSeq);
        if (idx >= 0)
        {
            out = jitter[idx];
            jitter[idx].used = false;
            ok = true;
        }
        xSemaphoreGive(jitterMutex);
    }

    return ok;
}

static bool insertPacketLocked(uint32_t seq,
                               uint32_t pts_samples,
                               uint16_t frame_samples,
                               uint16_t payload_len,
                               uint8_t flags,
                               const uint8_t *payload)
{
    bool inserted = false;

    if (payload_len > MAX_OPUS_PAYLOAD)
    {
        if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
        {
            droppedPackets++;
            malformedPackets++;
            xSemaphoreGive(jitterMutex);
        }
        return false;
    }

    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        if (findSlotBySeqNoLock(seq) >= 0)
        {
            duplicatePackets++;
        }
        else if (playbackStarted && !rebuffering && seq < expectedSeq)
        {
            latePackets++;
        }
        else
        {
            int idx = findFreeSlotNoLock();

            if (idx < 0)
            {
                int oldestIdx = findOldestSlotNoLock();

                if (oldestIdx >= 0 && seq > jitter[oldestIdx].seq)
                {
                    // keep newer data, discard oldest buffered packet
                    idx = oldestIdx;
                    jitter[idx].used = false;
                    droppedPackets++;
                    bufferFullDrops++;
                    evictedOldPackets++;
                }
                else
                {
                    // incoming packet is not better than what we already have
                    droppedPackets++;
                    bufferFullDrops++;
                    xSemaphoreGive(jitterMutex);
                    return false;
                }
            }

            jitter[idx].used = true;
            jitter[idx].seq = seq;
            jitter[idx].pts_samples = pts_samples;
            jitter[idx].frame_samples = frame_samples;
            jitter[idx].payload_len = payload_len;
            jitter[idx].flags = flags;
            memcpy(jitter[idx].payload, payload, payload_len);

            if (receivedPackets == 0 || seq > highestSeqSeen)
            {
                highestSeqSeen = seq;
            }

            receivedPackets++;
            lastPacketMillis = millis();
            inserted = true;
        }

        xSemaphoreGive(jitterMutex);
    }

    return inserted;
}

static void markEndPacket(uint32_t seq)
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        endSeen = true;
        endSeq = seq;
        xSemaphoreGive(jitterMutex);
    }
}

static void setPlaybackState(bool started, bool rebuf, bool finished, uint32_t consecMiss)
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        playbackStarted = started;
        rebuffering = rebuf;
        streamFinished = finished;
        consecutiveMisses = consecMiss;
        if (rebuf)
        {
            resumeLogArmed = true;
        }
        xSemaphoreGive(jitterMutex);
    }
}

static void setExpectedSeqAndResume(uint32_t seq)
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        expectedSeq = seq;
        playbackStarted = true;
        rebuffering = false;
        consecutiveMisses = 0;
        xSemaphoreGive(jitterMutex);
    }
}

static bool shouldPrintResumeLogAndDisarm()
{
    bool shouldPrint = false;
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        shouldPrint = resumeLogArmed;
        resumeLogArmed = false;
        xSemaphoreGive(jitterMutex);
    }
    return shouldPrint;
}

static void advanceExpectedSeq()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        expectedSeq++;
        xSemaphoreGive(jitterMutex);
    }
}

static void incrementMissingAndMaybeRebuffer(bool doRebuffer)
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        missingPackets++;
        consecutiveMisses++;
        if (doRebuffer)
        {
            rebuffering = true;
            resumeLogArmed = true;
        }
        xSemaphoreGive(jitterMutex);
    }
}

static void resetConsecutiveMisses()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        consecutiveMisses = 0;
        xSemaphoreGive(jitterMutex);
    }
}

static void incrementDecodeErrors()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        decodeErrors++;
        xSemaphoreGive(jitterMutex);
    }
}

static void incrementDecodedPackets()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        decodedPackets++;
        xSemaphoreGive(jitterMutex);
    }
}

static void incrementMalformedPackets()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        malformedPackets++;
        xSemaphoreGive(jitterMutex);
    }
}

static void incrementPlcPackets()
{
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        plcPackets++;
        xSemaphoreGive(jitterMutex);
    }
}

// ==============================
// Init
// ==============================
static bool i2sInit()
{
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    config.dma_buf_count = 8;
    config.dma_buf_len = 256;
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = 0;

    i2s_pin_config_t pins = {};
    pins.bck_io_num = I2S_BCK_PIN;
    pins.ws_io_num = I2S_WS_PIN;
    pins.data_out_num = I2S_DATA_PIN;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    esp_err_t err = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
    if (err != ESP_OK)
    {
        Serial.printf("[I2S] driver_install failed: %d\n", err);
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK)
    {
        Serial.printf("[I2S] set_pin failed: %d\n", err);
        return false;
    }

    err = i2s_zero_dma_buffer(I2S_PORT);
    if (err != ESP_OK)
    {
        Serial.printf("[I2S] zero_dma_buffer failed: %d\n", err);
        return false;
    }

    return true;
}

static bool opusInit()
{
    opusDecoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &opusErr);
    if (!opusDecoder || opusErr != OPUS_OK)
    {
        Serial.printf("[OPUS] decoder create failed: %d\n", opusErr);
        return false;
    }

    resetDecoderState();
    return true;
}

// ==============================
// UDP
// ==============================
static void sendStart()
{
    resetStreamState();

    if (xSemaphoreTake(jitterMutex, portMAX_DELAY) == pdTRUE)
    {
        startSentMillis = millis();
        xSemaphoreGive(jitterMutex);
    }

    udp.beginPacket(SERVER_IP, SERVER_PORT);
    udp.write((const uint8_t *)"START", 5);
    udp.endPacket();

    Serial.println("[UDP] START sent");
}

static bool parseAndQueuePacket(const uint8_t *buf, size_t len)
{
    if (len < HEADER_SIZE)
    {
        return false;
    }

    if (memcmp(buf, MAGIC, 4) != 0)
    {
        return false;
    }

    uint8_t version = buf[4];
    uint8_t flags = buf[5];
    uint32_t seq = read_be32(buf + 6);
    uint32_t pts_samples = read_be32(buf + 10);
    uint16_t frame_samples = read_be16(buf + 14);
    uint16_t payload_len = read_be16(buf + 16);

    if (version != VERSION)
    {
        return false;
    }

    if ((HEADER_SIZE + payload_len) != len)
    {
        return false;
    }

    if ((flags & FLAG_END) != 0)
    {
        markEndPacket(seq);
        Serial.printf("[UDP] END packet seq=%lu\n", (unsigned long)seq);
        return true;
    }

    if (frame_samples != FRAME_SAMPLES)
    {
        return false;
    }

    return insertPacketLocked(seq, pts_samples, frame_samples, payload_len, flags, buf + HEADER_SIZE);
}

// ==============================
// Audio output
// ==============================
static void writePcmToI2S(const int16_t *pcm, size_t samples)
{
    size_t bytesWritten = 0;
    i2s_write(I2S_PORT, pcm, samples * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
}

static void writeSilenceFrame()
{
    writePcmToI2S(silenceFrame, FRAME_SAMPLES);
}

static void decodeAndWritePLC()
{
    int plcSamples = opus_decode(
        opusDecoder,
        nullptr,
        0,
        pcmOut,
        FRAME_SAMPLES,
        0);

    if (plcSamples > 0)
    {
        writePcmToI2S(pcmOut, plcSamples);
        incrementPlcPackets();
    }
    else
    {
        writeSilenceFrame();
        incrementDecodeErrors();
    }
}

// ==============================
// Tasks
// ==============================
static void udpRxTask(void *parameter)
{
    static uint8_t rxBuf[HEADER_SIZE + MAX_OPUS_PAYLOAD + 16];

    while (true)
    {
        int packetSize = udp.parsePacket();
        if (packetSize > 0)
        {
            int n = udp.read(rxBuf, sizeof(rxBuf));
            if (n > 0)
            {
                bool ok = parseAndQueuePacket(rxBuf, (size_t)n);
                if (!ok)
                {
                    incrementMalformedPackets();
                }
            }
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

static void playbackTask(void *parameter)
{
    TickType_t lastWake = xTaskGetTickCount();

    while (true)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(PLAYBACK_PERIOD_MS));

        bool started, rebuf, seenEnd, finished;
        uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
        getStateSnapshot(started, rebuf, seenEnd, finished, expSeq, hiSeq, eSeq, lastPktMs, consecMiss);

        int buffered = getBufferedCountLocked();
        uint32_t minSeq = 0;
        bool foundMin = (buffered > 0) ? getMinBufferedSeqLocked(minSeq) : false;
        uint32_t nowMs = millis();

        if (finished)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (seenEnd && buffered == 0 && expSeq >= eSeq)
        {
            setPlaybackState(false, true, true, 0);
            Serial.println("[PLAYBACK] stream finished by END");
            continue;
        }

        if (!seenEnd && started && buffered == 0)
        {
            if ((nowMs - lastPktMs) > END_TIMEOUT_MS)
            {
                setPlaybackState(false, true, true, 0);
                Serial.println("[PLAYBACK] stream finished by timeout");
                continue;
            }
        }

        if (rebuf || !started)
        {
            bool canResume = false;
            int needed = started ? REBUFFER_PACKETS : START_BUFFERED_PACKETS;

            if (seenEnd)
            {
                canResume = (buffered > 0 && foundMin);
            }
            else
            {
                canResume = (buffered >= needed && foundMin);
            }

            if (canResume)
            {
                setExpectedSeqAndResume(minSeq);
                if (shouldPrintResumeLogAndDisarm())
                {
                    Serial.printf("[PLAYBACK] start/resume at seq=%lu buffered=%d\n",
                                  (unsigned long)minSeq, buffered);
                }
            }
            else
            {
                continue;
            }
        }

        if (!popExpectedPacketLocked(playbackPkt))
        {
            // rebuffer ONLY when the buffer is actually low,
            // or misses are too many AND the buffer is also low-ish.
            bool bufferIsActuallyLow = (buffered <= LOW_WATER_PACKETS);
            bool tooManyMissesWithLowBuffer = ((consecMiss + 1) >= MAX_CONSECUTIVE_MISSES) &&
                                              (buffered <= (REBUFFER_PACKETS / 2));

            bool doRebuffer = (!seenEnd && (bufferIsActuallyLow || tooManyMissesWithLowBuffer));

            incrementMissingAndMaybeRebuffer(doRebuffer);

            if (doRebuffer)
            {
                continue;
            }

            decodeAndWritePLC();
            advanceExpectedSeq();
            continue;
        }

        resetConsecutiveMisses();

        int decodedSamples = opus_decode(
            opusDecoder,
            playbackPkt.payload,
            playbackPkt.payload_len,
            pcmOut,
            FRAME_SAMPLES,
            0);

        if (decodedSamples < 0)
        {
            incrementDecodeErrors();
            decodeAndWritePLC();
            advanceExpectedSeq();
            continue;
        }

        writePcmToI2S(pcmOut, decodedSamples);
        incrementDecodedPackets();
        advanceExpectedSeq();
    }
}

static void controlTask(void *parameter)
{
    while (true)
    {
        bool started, rebuf, seenEnd, finished;
        uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
        getStateSnapshot(started, rebuf, seenEnd, finished, expSeq, hiSeq, eSeq, lastPktMs, consecMiss);

        uint32_t now = millis();
        bool noActivityYet = (!started && !finished && !seenEnd && (receivedPackets == 0));

        if (noActivityYet)
        {
            uint32_t elapsed = now - startSentMillis;
            if (elapsed >= START_RETRY_MS)
            {
                Serial.println("[CTRL] retry START");
                sendStart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void statsTask(void *parameter)
{
    while (true)
    {
        int buffered = getBufferedCountLocked();

        bool started, rebuf, seenEnd, finished;
        uint32_t expSeq, hiSeq, eSeq, lastPktMs, consecMiss;
        getStateSnapshot(started, rebuf, seenEnd, finished, expSeq, hiSeq, eSeq, lastPktMs, consecMiss);

        Serial.printf(
            "[STATS] rx=%lu dec=%lu plc=%lu miss=%lu drop=%lu fullDrop=%lu evict=%lu dup=%lu late=%lu malformed=%lu decErr=%lu buffered=%d expected=%lu highest=%lu end=%d rebuf=%d finished=%d\n",
            (unsigned long)receivedPackets,
            (unsigned long)decodedPackets,
            (unsigned long)plcPackets,
            (unsigned long)missingPackets,
            (unsigned long)droppedPackets,
            (unsigned long)bufferFullDrops,
            (unsigned long)evictedOldPackets,
            (unsigned long)duplicatePackets,
            (unsigned long)latePackets,
            (unsigned long)malformedPackets,
            (unsigned long)decodeErrors,
            buffered,
            (unsigned long)expSeq,
            (unsigned long)hiSeq,
            seenEnd ? 1 : 0,
            rebuf ? 1 : 0,
            finished ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));
    }
}

// ==============================
// Arduino
// ==============================
void setup()
{
    Serial.begin(115200);
    delay(1000);

    jitterMutex = xSemaphoreCreateMutex();
    if (jitterMutex == nullptr)
    {
        Serial.println("[RTOS] failed to create jitter mutex");
        while (true)
        {
            delay(1000);
        }
    }

    resetStreamState();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("[WIFI] connecting");
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(500);
    }

    WiFi.setSleep(false);
    Serial.println();
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());

    if (!udp.begin(LOCAL_UDP_PORT))
    {
        Serial.println("[UDP] begin failed");
        while (true)
        {
            delay(1000);
        }
    }

    if (!i2sInit())
    {
        while (true)
        {
            delay(1000);
        }
    }

    if (!opusInit())
    {
        while (true)
        {
            delay(1000);
        }
    }

    if (xTaskCreate(udpRxTask, "udpRxTask", 4096, nullptr, 3, &udpRxTaskHandle) != pdPASS)
    {
        Serial.println("[RTOS] failed to create udpRxTask");
        while (true)
        {
            delay(1000);
        }
    }

    if (xTaskCreate(playbackTask, "playbackTask", 12288, nullptr, 2, &playbackTaskHandle) != pdPASS)
    {
        Serial.println("[RTOS] failed to create playbackTask");
        while (true)
        {
            delay(1000);
        }
    }

    if (xTaskCreate(controlTask, "controlTask", 4096, nullptr, 2, &controlTaskHandle) != pdPASS)
    {
        Serial.println("[RTOS] failed to create controlTask");
        while (true)
        {
            delay(1000);
        }
    }

    if (xTaskCreate(statsTask, "statsTask", 4096, nullptr, 1, &statsTaskHandle) != pdPASS)
    {
        Serial.println("[RTOS] failed to create statsTask");
        while (true)
        {
            delay(1000);
        }
    }

    sendStart();
}

void loop()
{
    vTaskDelay(portMAX_DELAY);
}