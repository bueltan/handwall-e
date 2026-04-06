#include "network_udp.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>

#include "app_state.h"
#include "config.h"
#include "ui_main.h"
#include "esp_heap_caps.h"

static WiFiUDP udp;
static bool queuesClearedWhileDisconnected = false;

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
                char buffer[128];
                int len = udp.read(buffer, sizeof(buffer) - 1);

                if (len > 0)
                {
                    buffer[len] = 0;
                    String msg = String(buffer);

                    if (msg == "PONG")
                    {
                        lastPongTime = millis();
                        udpStatus = UDP_CONNECTED;

                        if (currentScreen == SCREEN_MAIN)
                            drawStatus("UDP OK");
                    }
                    else
                    {
                        if (currentScreen == SCREEN_MAIN)
                            drawStatus(msg.c_str());
                    }
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