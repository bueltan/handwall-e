#include "network_udp.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>

#include "app_state.h"
#include "config.h"
#include "ui_main.h"
#include "esp_heap_caps.h"
#include "audio_playback.h"

static WiFiUDP udp;
static bool queuesClearedWhileDisconnected = false;

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


void logHeap(const char *tag)
{
    Serial.printf("[%s] free=%u largest=%u\n",
                  tag,
                  ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static bool enqueuePacketToQueue(QueueHandle_t queue, const uint8_t *data, size_t len, bool isAudio, TickType_t waitTicks)
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
        if (!isAudio && currentScreen == SCREEN_MAIN)
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
        logHeap("queue send failed + freed");
        if (!isAudio && currentScreen == SCREEN_MAIN)
            drawLog("queue full");
        return false;
    }

    logHeap("enqueue success");
    return true;
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
        {
            return result;
        }

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

        if (currentScreen == SCREEN_MAIN)
            drawStatus("UDP OK");
        return;
    }

    if (msg.startsWith("{"))
    {
        Serial.printf("[UDP JSON RAW] %s\n", msg.c_str());

        int typeKeyPos = msg.indexOf("\"type\"");
        if (typeKeyPos < 0)
        {
            if (currentScreen == SCREEN_MAIN)
                drawLog("JSON missing type");
            return;
        }

        String typeValue = extractJsonStringValue(msg, "type");
        String valueText = extractJsonStringValue(msg, "value");

        Serial.printf("[UDP JSON TYPE] %s\n", typeValue.c_str());
        Serial.printf("[UDP JSON VALUE] %s\n", valueText.c_str());

        if (typeValue == "assistant_response")
        {
            if (valueText.length() > 0)
            {
                if (currentScreen == SCREEN_MAIN)
                {
                    drawIncomingUDP(valueText.c_str());
                    drawStatus("Assistant response received");
                }
            }
            else
            {
                if (currentScreen == SCREEN_MAIN)
                    drawLog("Assistant value empty");
            }
            return;
        }

        if (currentScreen == SCREEN_MAIN)
            drawLog("Unknown JSON UDP");
        return;
    }

    Serial.printf("[UDP TEXT] %s\n", msg.c_str());

    if (currentScreen == SCREEN_MAIN)
        drawStatus(msg.c_str());
}

void connectWiFi()
{
    if (wifiSSID.length() == 0)
    {
        drawStatus("Missing WiFi name");
        return;
    }

    drawStatus("Connecting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
    esp_wifi_set_ps(WIFI_PS_NONE);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
    {
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiStatus = WIFI_CONNECTED;
        udpStatus = DISCONNECTED;
        udpSocketStarted = false;
        lastPingTime = 0;
        lastPongTime = millis();
        queuesClearedWhileDisconnected = false;
        drawStatus("WiFi OK - waiting UDP");
    }
    else
    {
        wifiStatus = DISCONNECTED;
        udpStatus = DISCONNECTED;
        udpSocketStarted = false;
        resetPlaybackBuffer();
        drawStatus("WiFi FAILED");
    }
}

bool enqueueUDPAudio(const uint8_t *data, size_t len)
{
    return enqueuePacketToQueue(udpAudioQueue, data, len, true, pdMS_TO_TICKS(5));
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
            logHeap("before free control");
            free(pkt.data);
            pkt.data = NULL;
            logHeap("after free control");
        }
    }
}

static void sendPacket(const UdpPacket_t &pkt)
{
    if (wifiStatus != WIFI_CONNECTED)
        return;

    if (!udpSocketStarted)
        return;

    if (pkt.data == NULL || pkt.length == 0 || serverIP.length() == 0)
        return;

    udp.beginPacket(serverIP.c_str(), UDP_PORT);
    udp.write(pkt.data, pkt.length);
    udp.endPacket();

    if (!pkt.isAudio && currentScreen == SCREEN_MAIN)
    {
        drawLog("UDP sent");
    }
}

static void handleIncomingUdpRawPacket(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0)
        return;

    if (isLikelyTextPacket(data, len))
    {
        char textBuffer[512];
        int copyLen = len;

        if (copyLen >= (int)sizeof(textBuffer))
            copyLen = sizeof(textBuffer) - 1;

        memcpy(textBuffer, data, copyLen);
        textBuffer[copyLen] = '\0';

        String msg = String(textBuffer);
        handleIncomingUdpMessage(msg);
        return;
    }

    pushUdpAudioToPlayback(data, (size_t)len);

    if (currentScreen == SCREEN_MAIN)
    {
        drawStatus("Receiving audio");
    }
}
void taskUDP(void *pvParameters)
{
    UdpPacket_t pkt;

    while (true)
    {
        if (wifiStatus == WIFI_CONNECTED)
        {
            if (commitRequested)
            {
                logHeap("before COMMIT");
                commitRequested = false;

                if (enqueueUDPControl("COMMIT"))
                {
                    logHeap("after COMMIT enqueue");
                    if (currentScreen == SCREEN_MAIN)
                        drawStatus("Queued COMMIT");
                }
                else
                {
                    logHeap("after COMMIT enqueue");
                    if (currentScreen == SCREEN_MAIN)
                        drawStatus("Queue COMMIT failed");
                }
            }

            if (cancelRequested)
            {
                cancelRequested = false;
                resetPlaybackBuffer();
                if (enqueueUDPControl("CANCEL"))
                {
                    if (currentScreen == SCREEN_MAIN)
                        drawStatus("Queued CANCEL");
                }
                else
                {
                    if (currentScreen == SCREEN_MAIN)
                        drawStatus("Queue CANCEL failed");
                }
            }

            if (!udpSocketStarted)
            {
                logHeap("before udp.begin");
                if (udp.begin(UDP_PORT))
                {
                    logHeap("after udp.begin ok");
                    udpSocketStarted = true;
                    lastPongTime = millis();

                    if (currentScreen == SCREEN_MAIN)
                        drawLog("UDP socket ready");
                }
                else
                {
                    logHeap("after udp.begin failed");
                    if (currentScreen == SCREEN_MAIN)
                        drawLog("udp.begin failed");

                    vTaskDelay(500 / portTICK_PERIOD_MS);
                    continue;
                }
            }

            while (xQueueReceive(udpControlQueue, &pkt, 0) == pdPASS)
            {
                sendPacket(pkt);

                if (pkt.data != NULL)
                {
                    free(pkt.data);
                    pkt.data = NULL;
                }
            }

            if (xQueueReceive(udpAudioQueue, &pkt, 0) == pdPASS)
            {
                sendPacket(pkt);

                if (pkt.data != NULL)
                {
                    logHeap("before free audio");
                    free(pkt.data);
                    pkt.data = NULL;
                    logHeap("after free audio");
                }
            }

            if (currentScreen == SCREEN_MAIN && millis() - lastPingTime > PING_INTERVAL)
            {
                lastPingTime = millis();

                const char *pingMsg = "PING";
                udp.beginPacket(serverIP.c_str(), UDP_PORT);
                udp.write((const uint8_t *)pingMsg, strlen(pingMsg));
                udp.endPacket();
            }

            int packetSize = udp.parsePacket();
            if (packetSize > 0)
            {
                uint8_t rxBuffer[1472];
                int readLen = packetSize;

                if (readLen > (int)sizeof(rxBuffer))
                    readLen = sizeof(rxBuffer);

                int len = udp.read(rxBuffer, readLen);
                if (len > 0)
                {
                    handleIncomingUdpRawPacket(rxBuffer, len);
                }
            }

            if (currentScreen == SCREEN_MAIN && millis() - lastPongTime > PONG_TIMEOUT)
            {
                udpStatus = DISCONNECTED;
            }
        }
        else
        {
            udpStatus = DISCONNECTED;
            udpSocketStarted = false;

            if (!queuesClearedWhileDisconnected)
            {
                clearUDPAudioQueue();
                clearUDPControlQueue();
                queuesClearedWhileDisconnected = true;
            }
        }

        vTaskDelay(2 / portTICK_PERIOD_MS);
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
