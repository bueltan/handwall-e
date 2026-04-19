#include "network_udp.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <string.h>

#include "app_state.h"
#include "audio_playback.h"
#include "config.h"
#include "display_hal.h"
#include "ui_dev.h"
#include "ui_start.h"

static WiFiUDP udp;
static SemaphoreHandle_t udpMutex = NULL;

static bool queuesClearedWhileDisconnected = false;
static bool pendingCommit = false;
static uint32_t lastIncomingAudioUiMs = 0;

// ============================================================
// Helpers
// ============================================================

static uint16_t readBe16(const uint8_t *p)
{
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static uint32_t readBe32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) |
           (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}

static bool enqueuePacketToQueue(
    QueueHandle_t queue,
    const uint8_t *data,
    size_t len,
    bool isAudio,
    TickType_t waitTicks)
{
    if (queue == NULL || data == NULL || len == 0)
        return false;

    if (wifiStatus != WIFI_CONNECTED)
        return false;

    if (serverIP.length() == 0)
        return false;

    uint8_t *buffer = (uint8_t *)malloc(len);
    if (buffer == NULL)
    {
        if (!isAudio && currentScreen == SCREEN_DEV)
            drawLog("malloc failed");
        return false;
    }

    memcpy(buffer, data, len);

    UdpPacket_t pkt;
    pkt.data = buffer;
    pkt.length = len;
    pkt.isAudio = isAudio;

    if (xQueueSend(queue, &pkt, waitTicks) != pdPASS)
    {
        free(buffer);

        if (!isAudio && currentScreen == SCREEN_DEV)
            drawLog("queue full");

        return false;
    }

    return true;
}

static bool isLikelyTextPacket(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0)
        return false;

    for (int i = 0; i < len; ++i)
    {
        uint8_t c = data[i];

        if (c == 0)
            return false;

        if (c == '\n' || c == '\r' || c == '\t')
            continue;

        if (c < 32 || c > 126)
            return false;
    }

    return true;
}

static bool isLikelyOpusPacket(const uint8_t *data, int len)
{
    if (data == NULL || len < (int)sizeof(OpusUdpHeader))
        return false;

    return memcmp(data, OPUS_MAGIC_BYTES, 4) == 0;
}

static String extractJsonStringValue(const String &json, const char *key)
{
    String keyToken = String("\"") + key + "\"";
    int keyPos = json.indexOf(keyToken);
    if (keyPos < 0)
        return "";

    int colonPos = json.indexOf(':', keyPos + keyToken.length());
    if (colonPos < 0)
        return "";

    int firstQuote = json.indexOf('"', colonPos + 1);
    if (firstQuote < 0)
        return "";

    String result = "";
    bool escape = false;

    for (int i = firstQuote + 1; i < json.length(); ++i)
    {
        char c = json[i];

        if (escape)
        {
            switch (c)
            {
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            case '"':
                result += '"';
                break;
            case '\\':
                result += '\\';
                break;
            default:
                result += c;
                break;
            }
            escape = false;
            continue;
        }

        if (c == '\\')
        {
            escape = true;
            continue;
        }

        if (c == '"')
            return result;

        result += c;
    }

    return "";
}

static void handleIncomingUdpMessage(const String &msg)
{
    if (msg == "PONG")
    {
        lastPongTime = millis();
        udpStatus = UDP_CONNECTED;

        if (currentScreen == SCREEN_DEV)
            drawStatus("UDP OK");

        drawStartConnectionStatus();
        return;
    }

    if (msg.startsWith("{") && msg.indexOf("\"type\"") >= 0)
    {
        String type = extractJsonStringValue(msg, "type");
        String value = extractJsonStringValue(msg, "value");

        if (type == "assistant_response")
        {
            if (currentScreen == SCREEN_DEV)
            {
                drawIncomingUDP(value.c_str());
                drawStatus("Assistant response");
            }

            if (currentScreen == SCREEN_START)
                updateStartAudioLevel(0, AUDIO_BAR_INCOMING);

            return;
        }

        if (type == "bridge_status")
        {
            if (value == "xai_connected")
            {
                udpStatus = UDP_CONNECTED;

                if (currentScreen == SCREEN_DEV)
                    drawStatus("xAI connected");
            }
            else if (value == "xai_disconnected")
            {
                if (currentScreen == SCREEN_DEV)
                    drawStatus("xAI disconnected");
            }
            else
            {
                if (currentScreen == SCREEN_DEV)
                    drawStatus(value.c_str());
            }

            drawStartConnectionStatus();
            return;
        }

        if (currentScreen == SCREEN_DEV)
            drawLog("Unknown JSON UDP");

        return;
    }

    if (currentScreen == SCREEN_DEV)
        drawLog(msg.c_str());
}

static bool udpBeginLocked(uint16_t port)
{
    if (udpMutex == NULL)
        return false;

    if (xSemaphoreTake(udpMutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;

    bool ok = udp.begin(port);
    xSemaphoreGive(udpMutex);
    return ok;
}

static bool sendPacket(const UdpPacket_t &pkt)
{
    if (wifiStatus != WIFI_CONNECTED)
        return false;

    if (!udpSocketStarted)
        return false;

    if (pkt.data == NULL || pkt.length == 0 || serverIP.length() == 0)
        return false;

    if (udpMutex == NULL)
        return false;

    if (xSemaphoreTake(udpMutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return false;

    udp.beginPacket(serverIP.c_str(), UDP_PORT);
    udp.write(pkt.data, pkt.length);
    int ok = udp.endPacket();

    xSemaphoreGive(udpMutex);

    if (!ok)
    {
        Serial.printf("[UDP] sendPacket failed len=%u\n", (unsigned)pkt.length);
        return false;
    }

    if (!pkt.isAudio && currentScreen == SCREEN_DEV)
        drawLog("UDP sent");

    return true;
}

static bool sendPing()
{
    if (wifiStatus != WIFI_CONNECTED || !udpSocketStarted || serverIP.length() == 0)
        return false;

    if (udpMutex == NULL)
        return false;

    if (xSemaphoreTake(udpMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;

    const char *pingMsg = "PING";
    udp.beginPacket(serverIP.c_str(), UDP_PORT);
    udp.write((const uint8_t *)pingMsg, strlen(pingMsg));
    int ok = udp.endPacket();

    xSemaphoreGive(udpMutex);

    if (!ok)
    {
        Serial.println("[UDP] PING send failed");
        return false;
    }

    return true;
}

static void handleIncomingUdpRawPacket(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0)
        return;

    // 1) Text / JSON
    if (isLikelyTextPacket(data, len))
    {
        char textBuffer[512];
        int copyLen = len;

        if (copyLen >= (int)sizeof(textBuffer))
            copyLen = (int)sizeof(textBuffer) - 1;

        memcpy(textBuffer, data, copyLen);
        textBuffer[copyLen] = '\0';

        String msg(textBuffer);
        handleIncomingUdpMessage(msg);
        return;
    }

    // 2) Opus packets
    if (isLikelyOpusPacket(data, len))
    {
        if (len < (int)sizeof(OpusUdpHeader))
        {
            if (currentScreen == SCREEN_DEV)
                drawLog("Opus hdr short");
            return;
        }

        const OpusUdpHeader *hdr = reinterpret_cast<const OpusUdpHeader *>(data);

        if (hdr->version != OPUS_VERSION)
        {
            if (currentScreen == SCREEN_DEV)
                drawLog("Opus ver bad");
            return;
        }

        const uint32_t sequence =
            readBe32(reinterpret_cast<const uint8_t *>(&hdr->sequence_be));
        const uint32_t ptsSamples =
            readBe32(reinterpret_cast<const uint8_t *>(&hdr->pts_samples_be));
        const uint16_t frameSamples =
            readBe16(reinterpret_cast<const uint8_t *>(&hdr->frame_samples_be));
        const uint16_t payloadLen =
            readBe16(reinterpret_cast<const uint8_t *>(&hdr->payload_len_be));

        // ✅ IMPORTANTE: no resetear aquí; solo marcar END
        if ((hdr->flags & OPUS_FLAG_END) != 0)
        {
            markOutputAudioEnd(sequence);

            if (currentScreen == SCREEN_DEV)
                drawStatus("Opus END");

            return;
        }

        if (frameSamples != OUTPUT_OPUS_FRAME_SAMPLES)
        {
            if (currentScreen == SCREEN_DEV)
                drawLog("frame!=320");
            return;
        }

        if ((int)sizeof(OpusUdpHeader) + payloadLen != len)
        {
            if (currentScreen == SCREEN_DEV)
                drawLog("Opus size bad");
            return;
        }

        const uint8_t *payload = data + sizeof(OpusUdpHeader);

        bool ok = registerOutputOpusPacket(
            sequence,
            ptsSamples,
            frameSamples,
            payload,
            payloadLen);

        if (ok)
        {
            if (currentScreen == SCREEN_START)
                updateStartAudioLevel(0, AUDIO_BAR_INCOMING);

            if (currentScreen == SCREEN_DEV)
            {
                uint32_t now = millis();
                if (now - lastIncomingAudioUiMs >= 300)
                {
                    lastIncomingAudioUiMs = now;

                    char msg[96];
                    snprintf(
                        msg,
                        sizeof(msg),
                        "opus seq=%lu pts=%lu",
                        (unsigned long)sequence,
                        (unsigned long)ptsSamples);
                    drawLog(msg);
                    drawStatus("Receiving Opus");
                }
            }
        }
        else
        {
            if (currentScreen == SCREEN_DEV)
                drawLog("Opus drop");
        }

        return;
    }

    if (currentScreen == SCREEN_DEV)
        drawLog("Unknown UDP bin");
}

// ============================================================
// Public API
// ============================================================

void connectWiFi()
{
    if (wifiSSID.length() == 0)
    {
        if (currentScreen == SCREEN_DEV)
            drawStatus("Missing WiFi name");
        return;
    }

    if (currentScreen == SCREEN_DEV)
        drawStatus("Connecting WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        esp_wifi_set_ps(WIFI_PS_NONE);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);

        wifiStatus = WIFI_CONNECTED;
        udpStatus = DISCONNECTED;
        udpSocketStarted = false;
        lastPingTime = 0;
        lastPongTime = millis();
        queuesClearedWhileDisconnected = false;
        pendingCommit = false;

        resetOutputPacketJitter();
        resetPlaybackBuffer();

        if (currentScreen == SCREEN_DEV)
            drawStatus("WiFi OK");

        drawStartConnectionStatus();
    }
    else
    {
        wifiStatus = DISCONNECTED;
        udpStatus = DISCONNECTED;
        udpSocketStarted = false;
        pendingCommit = false;

        resetPlaybackBuffer();
        resetOutputPacketJitter();

        if (currentScreen == SCREEN_DEV)
            drawStatus("WiFi FAILED");

        drawStartConnectionStatus();
    }
}

bool enqueueUDPAudio(const uint8_t *data, size_t len)
{
    return enqueuePacketToQueue(
        udpAudioQueue,
        data,
        len,
        true,
        pdMS_TO_TICKS(5));
}

bool enqueueUDPControl(const String &msg)
{
    return enqueuePacketToQueue(
        udpControlQueue,
        (const uint8_t *)msg.c_str(),
        msg.length(),
        false,
        pdMS_TO_TICKS(50));
}

void clearUDPAudioQueue()
{
    if (udpAudioQueue == NULL)
        return;

    UdpPacket_t pkt;
    while (xQueueReceive(udpAudioQueue, &pkt, 0) == pdPASS)
    {
        if (pkt.data != NULL)
        {
            free(pkt.data);
            pkt.data = NULL;
        }
    }
}

void clearUDPControlQueue()
{
    if (udpControlQueue == NULL)
        return;

    UdpPacket_t pkt;
    while (xQueueReceive(udpControlQueue, &pkt, 0) == pdPASS)
    {
        if (pkt.data != NULL)
        {
            free(pkt.data);
            pkt.data = NULL;
        }
    }
}

void udpRxTask(void *pvParameters)
{
    (void)pvParameters;

    static uint8_t rxBuffer[1472];

    if (udpMutex == NULL)
        udpMutex = xSemaphoreCreateMutex();

    for (;;)
    {
        if (wifiStatus != WIFI_CONNECTED || !udpSocketStarted || udpMutex == NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        bool receivedAny = false;

        for (;;)
        {
            int len = 0;

            if (xSemaphoreTake(udpMutex, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                int packetSize = udp.parsePacket();

                if (packetSize > 0)
                {
                    int readLen = packetSize;
                    if (readLen > (int)sizeof(rxBuffer))
                        readLen = (int)sizeof(rxBuffer);

                    len = udp.read(rxBuffer, readLen);
                }

                xSemaphoreGive(udpMutex);
            }

            if (len <= 0)
                break;

            handleIncomingUdpRawPacket(rxBuffer, len);
            receivedAny = true;
        }

        if (!receivedAny)
            vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void taskUDP(void *pvParameters)
{
    (void)pvParameters;

    UdpPacket_t pkt;
    bool commitPendingAfterDrain = false;
    uint32_t lastPingMs = 0;

    if (udpMutex == NULL)
        udpMutex = xSemaphoreCreateMutex();

    for (;;)
    {
        if (wifiStatus != WIFI_CONNECTED)
        {
            udpStatus = DISCONNECTED;
            udpSocketStarted = false;
            commitRequested = false;
            cancelRequested = false;
            commitPendingAfterDrain = false;

            if (!queuesClearedWhileDisconnected)
            {
                clearUDPAudioQueue();
                clearUDPControlQueue();
                resetPlaybackBuffer();
                resetOutputPacketJitter();
                queuesClearedWhileDisconnected = true;
                Serial.println("[UDP] disconnected -> queues cleared");
            }

            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        queuesClearedWhileDisconnected = false;

        if (!udpSocketStarted)
        {
            bool ok = false;

            if (udpMutex && xSemaphoreTake(udpMutex, pdMS_TO_TICKS(200)) == pdTRUE)
            {
                ok = udp.begin(UDP_PORT);
                xSemaphoreGive(udpMutex);
            }

            if (ok)
            {
                udpSocketStarted = true;
                lastPongTime = millis();
                lastPingMs = millis();
                pendingCommit = false;

                Serial.printf("[UDP] socket ready on local port %d\n", UDP_PORT);

                if (currentScreen == SCREEN_DEV)
                    drawLog("UDP socket ready");
            }
            else
            {
                Serial.println("[UDP] udp.begin failed");

                if (currentScreen == SCREEN_DEV)
                    drawLog("udp.begin failed");

                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        if (cancelRequested)
        {
            cancelRequested = false;
            commitRequested = false;
            commitPendingAfterDrain = false;
            pendingCommit = false;

            resetPlaybackBuffer();
            resetOutputPacketJitter();
            clearUDPAudioQueue();

            Serial.println("[UDP] CANCEL requested");

            if (enqueueUDPControl("CANCEL"))
            {
                if (currentScreen == SCREEN_DEV)
                    drawStatus("Queued CANCEL");
            }
            else
            {
                if (currentScreen == SCREEN_DEV)
                    drawStatus("Queue CANCEL failed");
            }
        }

        if (commitRequested)
        {
            commitRequested = false;
            micStreaming = false;
            commitPendingAfterDrain = true;
            pendingCommit = true;

            Serial.println("[UDP] COMMIT pending after drain (mic off)");

            if (currentScreen == SCREEN_DEV)
                drawStatus("Commit pending drain");
        }

        int audioPacketsSent = 0;
        const int maxAudioPacketsPerLoop = commitPendingAfterDrain ? 4 : 1;

        while (audioPacketsSent < maxAudioPacketsPerLoop &&
               xQueueReceive(udpAudioQueue, &pkt, 0) == pdPASS)
        {
            if (sendPacket(pkt))
                audioPacketsSent++;

            if (pkt.data != NULL)
            {
                free(pkt.data);
                pkt.data = NULL;
            }
        }

        if (commitPendingAfterDrain && uxQueueMessagesWaiting(udpAudioQueue) == 0)
        {
            resetPlaybackBuffer();
            resetOutputPacketJitter();

            Serial.println("[UDP] enqueue COMMIT");

            if (enqueueUDPControl("COMMIT"))
            {
                if (currentScreen == SCREEN_DEV)
                    drawStatus("Queued COMMIT");
            }
            else
            {
                if (currentScreen == SCREEN_DEV)
                    drawStatus("Queue COMMIT failed");
            }

            commitPendingAfterDrain = false;
            pendingCommit = false;
        }


        int controlPacketsSent = 0;
        while (xQueueReceive(udpControlQueue, &pkt, 0) == pdPASS)
        {
            if (sendPacket(pkt))
                controlPacketsSent++;

            if (pkt.data != NULL)
            {
                free(pkt.data);
                pkt.data = NULL;
            }
        }

        if (controlPacketsSent > 0)
        {
            Serial.printf("[UDP] sent %d control packet(s)\n", controlPacketsSent);
        }

        if (millis() - lastPingMs >= PING_INTERVAL)
        {
            lastPingMs = millis();

        }

        if (millis() - lastPongTime > PONG_TIMEOUT)
        {
            if (udpStatus != DISCONNECTED)
            {
                udpStatus = DISCONNECTED;
                Serial.println("[UDP] PONG timeout");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }}
