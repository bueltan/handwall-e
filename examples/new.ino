#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <driver/i2s.h>
#include <esp_wifi.h>

// ====================== PINOUT ======================
#define I2S_WS 17
#define I2S_SCK 4
#define I2S_SD 16
#define I2S_MIC_PORT I2S_NUM_0
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// ====================== OBJETOS ======================
XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button btnWifi, btnSend, btnMic, btnCommit, btnCancel;
WiFiUDP udp;

// ====================== ESTADOS ======================
enum Status { DISCONNECTED, WIFI_CONNECTED, UDP_CONNECTED };
Status wifiStatus = DISCONNECTED;
Status udpStatus = DISCONNECTED;
bool micStreaming = false;

// ====================== CONFIG ======================
const char* ssid = "Galaxy M12 C9A6";
const char* password = "zhof1469";
const char* udpAddress = "10.19.187.13";
const int udpPort = 5000;
const int font_size_s = 1;

// Audio
#define SAMPLE_RATE 16000
#define SAMPLES_PER_PACKET 160
#define BUFFER_LEN SAMPLES_PER_PACKET
#define PACKET_SIZE (BUFFER_LEN * 2)

// ====================== HEADER ======================
typedef struct __attribute__((packed)) {
    uint32_t sequence;
} AudioHeader;

// ====================== QUEUE PARA UDP (ASÍNCRONO) ======================
QueueHandle_t udpSendQueue = NULL;

typedef struct {
    uint8_t* data;
    size_t   length;
    bool     isAudio;   // true = paquete de audio, false = comando/texto
} UdpPacket_t;

// ====================== VARIABLES ======================
uint32_t sequence = 0;
uint8_t packet[sizeof(AudioHeader) + PACKET_SIZE];
int32_t i2sBuffer[BUFFER_LEN];
uint8_t udpPacket[PACKET_SIZE];

unsigned long lastPingTime = 0;
unsigned long lastPongTime = 0;
const unsigned long pingInterval = 2000;
const unsigned long pongTimeout = 3000;

SemaphoreHandle_t screenMutex = NULL;

// ====================== PANTALLA ======================


void drawUI() {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.fillScreen(TFT_BLACK);
    
    btnWifi.initButton(&tft, 60, 35, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"WiFi", font_size_s);
    btnWifi.drawButton(false);
    btnSend.initButton(&tft, 180, 35, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"SEND", font_size_s);
    btnSend.drawButton(false);
    btnMic.initButton(&tft, 45, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"MIC OFF", font_size_s);
    btnMic.drawButton(false);
    btnCancel.initButton(&tft, 120, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"CANCEL", font_size_s);
    btnCancel.drawButton(false);
    btnCommit.initButton(&tft, 195, 290, 60, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, (char*)"COMMIT", font_size_s);
    btnCommit.drawButton(false);

    tft.drawRect(10, 70, 220, 80, TFT_GREEN);
    tft.setCursor(20, 80);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(font_size_s);
    tft.println("Audio Level");

    tft.setCursor(20, 170);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(font_size_s);
    tft.println("Logs:");
    xSemaphoreGive(screenMutex);
}

void drawStatus(const char* msg) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.fillRect(15, 200, 300, 50, TFT_BLACK);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(1);
    tft.setCursor(20, 210);
    tft.println(msg);

    tft.setCursor(20, 245);
    tft.printf("WiFi:%s | UDP:%s | Mic:%s",
               (wifiStatus==WIFI_CONNECTED)?"OK":"NO",
               (udpStatus==UDP_CONNECTED)?"OK":"NO",
               micStreaming?"ON":"OFF");
    xSemaphoreGive(screenMutex);
}

void drawVolumeBar(int16_t volume) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    int barWidth = map(constrain(volume, 0, 32767), 0, 32767, 0, 200);
    tft.fillRect(20, 110, 200, 25, TFT_BLACK);
    tft.fillRect(20, 110, barWidth, 25, TFT_GREEN);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(20, 140);
    tft.setTextSize(font_size_s);
    tft.printf("Vol: %5d", volume);
    xSemaphoreGive(screenMutex);
}

void drawLog(const char* msg) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(font_size_s);
    tft.setCursor(20, 215);
    tft.println(msg);
    xSemaphoreGive(screenMutex);
}

// ====================== I2S ======================
void initI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 10,
        .dma_buf_len = BUFFER_LEN,
        .use_apll = false
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };
    i2s_driver_install(I2S_MIC_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_MIC_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_MIC_PORT);
}

// ====================== TAREA UDP SENDER (ASÍNCRONA) ======================
void taskUDPSender(void *pvParameters) {
    UdpPacket_t pkt;
    
    while (true) {
        if (xQueueReceive(udpSendQueue, &pkt, portMAX_DELAY) == pdPASS) {
            if (wifiStatus == WIFI_CONNECTED && pkt.data != NULL) {
                udp.beginPacket(udpAddress, udpPort);
                udp.write(pkt.data, pkt.length);
                udp.endPacket();
                
                // Loguear solo comandos (no audio, para no saturar la pantalla)
                if (!pkt.isAudio) {
                    drawLog("UDP sent");
                }
            }
            // Liberar memoria siempre
            if (pkt.data != NULL) {
                free(pkt.data);
                pkt.data = NULL;
            }
        }
    }
}

// ====================== TAREA MIC + STREAM ======================
void taskMicStream(void *pvParameters) {
    initI2S();
    int16_t maxVolume = 0;
    uint32_t lastPrint = 0;

    while (true) {
        if (!micStreaming || wifiStatus != WIFI_CONNECTED) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t result = i2s_read(I2S_MIC_PORT, i2sBuffer, sizeof(i2sBuffer), &bytes_read, portMAX_DELAY);
        if (result != ESP_OK || bytes_read == 0) {
            vTaskDelay(10);
            continue;
        }

        int samples = bytes_read / sizeof(int32_t);
        if (samples != SAMPLES_PER_PACKET) {
            vTaskDelay(2);
            continue;
        }

        maxVolume = 0;
        for (int i = 0; i < samples; i++) {
            int32_t raw = i2sBuffer[i];
            int16_t sample = (int16_t)(raw >> 10);
            if (abs(sample) > maxVolume) maxVolume = abs(sample);
            udpPacket[i*2]     = (uint8_t)(sample & 0xFF);
            udpPacket[i*2 + 1] = (uint8_t)(sample >> 8);
        }

        AudioHeader header;
        header.sequence = sequence++;

        memcpy(packet, &header, sizeof(AudioHeader));
        memcpy(packet + sizeof(AudioHeader), udpPacket, samples*2);

        // Enviar por la queue (asíncrono)
        UdpPacket_t audioPkt;
        audioPkt.data = (uint8_t*)malloc(sizeof(AudioHeader) + samples*2);
        if (audioPkt.data != NULL) {
            memcpy(audioPkt.data, packet, sizeof(AudioHeader) + samples*2);
            audioPkt.length = sizeof(AudioHeader) + samples*2;
            audioPkt.isAudio = true;

            if (xQueueSend(udpSendQueue, &audioPkt, pdMS_TO_TICKS(10)) != pdPASS) {
                free(audioPkt.data);  // descartar si la cola está llena
            }
        }

        drawVolumeBar(maxVolume);

        if (millis() - lastPrint > 800) {
            lastPrint = millis();
            char logMsg[100];
            snprintf(logMsg, sizeof(logMsg), "Sent seq=%lu smp=%d peak=%d", header.sequence, samples, maxVolume);
            drawLog(logMsg);
        }

        vTaskDelay(1);
    }
}

// ====================== TAREA TOUCH ======================
void taskTouch(void *pvParameters) {
    while (1) {
        TouchPoint p = ts.getTouch();
        int x = constrain(map(p.y, 220, 20, 0, 239), 0, 239);
        int y = constrain(map(p.x, 20, 299, 0, 319), 0, 319);

        if (p.zRaw > 200) {
            if (btnWifi.contains(x, y)) {
                btnWifi.drawButton(true);
                connectWiFi();
                vTaskDelay(300/portTICK_PERIOD_MS);
                btnWifi.drawButton(false);
            }
            if (btnSend.contains(x, y)) {
                btnSend.drawButton(true);
                sendUDP("HELLO_FROM_ESP32 | t=" + String(millis()));
                vTaskDelay(300/portTICK_PERIOD_MS);
                btnSend.drawButton(false);
            }
            if (btnMic.contains(x, y)) {
                btnMic.drawButton(true);
                micStreaming = !micStreaming;
                
                char micLabel[10];
                if (micStreaming) strcpy(micLabel, "MIC ON");
                else strcpy(micLabel, "MIC OFF");
                
                btnMic.initButton(&tft, 20, 260, 100, 40, TFT_GREEN, TFT_BLACK, TFT_GREEN, micLabel, 2);
                btnMic.drawButton(false);
                drawStatus(micStreaming?"Audio streaming STARTED":"Audio streaming STOPPED");
                vTaskDelay(250/portTICK_PERIOD_MS);
            }
            if (btnCommit.contains(x, y)) {
                btnCommit.drawButton(true);
                sendUDP("COMMIT");
                vTaskDelay(200/portTICK_PERIOD_MS);
                btnCommit.drawButton(false);
                drawStatus("Sent COMMIT");
            }
            if (btnCancel.contains(x, y)) {
                btnCancel.drawButton(true);
                sendUDP("CANCEL");
                vTaskDelay(200/portTICK_PERIOD_MS);
                btnCancel.drawButton(false);
                drawStatus("Sent CANCEL");
            }
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// ====================== TAREA UDP (PING/PONG) ======================
void taskUDP(void *pvParameters) {
    while (1) {
        if (wifiStatus == WIFI_CONNECTED) {
            if (millis() - lastPingTime > pingInterval) {
                lastPingTime = millis();
                sendUDP("PING");   // ahora también va por la queue
            }

            int packetSize = udp.parsePacket();
            if (packetSize) {
                char buffer[64];
                int len = udp.read(buffer, sizeof(buffer)-1);
                if (len > 0) buffer[len] = 0;
                String msg = String(buffer);
                
                if (msg == "PONG") {
                    lastPongTime = millis();
                    udpStatus = UDP_CONNECTED;
                    drawStatus("UDP OK");
                } else {
                    drawStatus(msg.c_str());
                }
            }

            if (millis() - lastPongTime > pongTimeout) {
                udpStatus = DISCONNECTED;
                drawStatus("UDP LOST");
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// ====================== WIFI ======================
void connectWiFi() {
    WiFi.begin(ssid, password);
    esp_wifi_set_ps(WIFI_PS_NONE);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-start < 15000) {
        vTaskDelay(200/portTICK_PERIOD_MS);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        wifiStatus = WIFI_CONNECTED;
        udp.begin(udpPort);
        udpStatus = UDP_CONNECTED;
        drawStatus("WiFi OK - PS_NONE");
    } else {
        drawStatus("WiFi FAILED");
    }
}

void sendUDP(String msg) {
    if (wifiStatus != WIFI_CONNECTED || udpSendQueue == NULL) return;
    
    size_t len = msg.length();
    uint8_t* buffer = (uint8_t*)malloc(len + 1);
    if (buffer == NULL) return;
    
    memcpy(buffer, msg.c_str(), len);
    buffer[len] = 0;
    
    UdpPacket_t pkt = {buffer, len, false};  // isAudio = false
    
    if (xQueueSend(udpSendQueue, &pkt, pdMS_TO_TICKS(50)) != pdPASS) {
        free(buffer);   // liberar si no se pudo encolar
        drawLog("UDP queue full!");
    }
}

// ====================== SETUP ======================
void setup() {
    ts.begin();
    tft.init();
    tft.setRotation(0);
    
    screenMutex = xSemaphoreCreateMutex();
    
    // Crear la cola para envíos UDP (20 paquetes máximo)
    udpSendQueue = xQueueCreate(20, sizeof(UdpPacket_t));
    if (udpSendQueue == NULL) {
        Serial.println("Error creando UDP Queue!");
    }

    drawUI();

    // Tareas
    xTaskCreatePinnedToCore(taskTouch,     "TouchTask",  4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(taskMicStream, "MicStream",  8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(taskUDP,       "UDPTask",    4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(taskUDPSender, "UDPSender",  6144, NULL, 3, NULL, 1);  // prioridad alta
}

// ====================== LOOP ======================
void loop() {
    // vacío - todo corre en tareas FreeRTOS
}