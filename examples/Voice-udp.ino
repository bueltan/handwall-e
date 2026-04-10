// ====================== CÓDIGO COMPLETO ESP32 (VERSIÓN ACTUALIZADA) ======================
// Cambios importantes para solucionar "solo llega 1 paquete":
// • i2s_read con portMAX_DELAY (más estable)
// • Cambio de shift: raw >> 13 → raw >> 16 (más común con micrófonos I2S 32-bit)
// • Logs más claros en pantalla ("Sent seq=XXX")
// • WiFi Power Saving desactivado (WIFI_PS_NONE)
// • Paquetes de 10ms (160 muestras)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <driver/i2s.h>
#include <esp_wifi.h>          // Para desactivar power saving

// ====================== PINOUT ======================
#define I2S_WS  17
#define I2S_SCK 4
#define I2S_SD  16
#define I2S_MIC_PORT I2S_NUM_0

// Pines Touch
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ====================== OBJETOS ======================
XPT2046_Bitbang ts(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button btnWifi;
TFT_eSPI_Button btnSend;
TFT_eSPI_Button btnMic;
WiFiUDP udp;

// ====================== ESTADOS ======================
enum Status { DISCONNECTED, WIFI_CONNECTED, UDP_CONNECTED };
Status wifiStatus = DISCONNECTED;
Status udpStatus = DISCONNECTED;
bool micStreaming = false;

// ====================== CONFIG ======================
const char* ssid = "Galaxy M12 C9A6"; //"MOVISTAR-WIFI6-B700";
const char* password = "zhof1469";//"uDRsVe6jpvSfSCiQ9wPL";
const char* udpAddress = "10.230.143.13"; //"192.168.1.33";   // ← Cambia por la IP de tu PC
const int udpPort = 5000;

// Audio (10ms por paquete)
#define SAMPLE_RATE         16000
#define SAMPLES_PER_PACKET  160
#define BUFFER_LEN          SAMPLES_PER_PACKET
#define PACKET_SIZE         (BUFFER_LEN * 2)

// ====================== HEADER ======================
typedef struct __attribute__((packed)) {
    uint32_t sequence;
} AudioHeader;

// ====================== VARIABLES ======================
uint32_t sequence = 0;

uint8_t packet[sizeof(AudioHeader) + PACKET_SIZE];
int32_t i2sBuffer[BUFFER_LEN];
uint8_t udpPacket[PACKET_SIZE];

unsigned long lastPingTime = 0;
unsigned long lastPongTime = 0;
const unsigned long pingInterval = 2000;
const unsigned long pongTimeout = 3000;

SemaphoreHandle_t screenMutex;

// ====================== PANTALLA ======================
void drawUI() {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.fillScreen(TFT_BLACK);
    btnWifi.initButton(&tft, 60, 35, 110, 45, TFT_WHITE, TFT_BLUE, TFT_WHITE, (char*)"WiFi", 2);
    btnWifi.drawButton(false);
    btnSend.initButton(&tft, 180, 35, 110, 45, TFT_WHITE, TFT_GREEN, TFT_WHITE, (char*)"SEND", 2);
    btnSend.drawButton(false);
    btnMic.initButton(&tft, 120, 280, 160, 45, TFT_WHITE, TFT_RED, TFT_WHITE, (char*)"MIC OFF", 2);
    btnMic.drawButton(false);

    tft.drawRect(10, 90, 220, 80, TFT_CYAN);
    tft.setCursor(20, 100);
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(2);
    tft.println("Audio Level");

    tft.drawRect(10, 180, 220, 90, TFT_WHITE);
    tft.setCursor(20, 190);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.println("UDP Logs:");
    xSemaphoreGive(screenMutex);
}

void drawStatus() {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.fillRect(0, 0, TFT_WIDTH, 30, TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(5, 8);
    tft.print("WiFi: ");
    tft.print((wifiStatus == WIFI_CONNECTED) ? "OK" : "NO");
    tft.print(" | UDP: ");
    tft.print((udpStatus == UDP_CONNECTED) ? "OK" : "NO");
    tft.setCursor(5, 20);
    tft.print("Mic: ");
    tft.print(micStreaming ? "STREAMING" : "OFF");
    xSemaphoreGive(screenMutex);
}

void drawVolumeBar(int16_t volume) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    int barWidth = map(constrain(volume, 0, 32767), 0, 32767, 0, 200);
    tft.fillRect(20, 125, 200, 25, TFT_BLACK);
    tft.fillRect(20, 125, barWidth, 25, TFT_GREEN);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(20, 155);
    tft.setTextSize(2);
    tft.printf("Vol: %5d", volume);
    xSemaphoreGive(screenMutex);
}

void drawLog(const char* msg) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.fillRect(15, 215, 210, 50, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(1);
    tft.setCursor(20, 220);
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
    Serial.println("I2S Mic OK (10ms packets)");
}

// ====================== TAREA MIC + STREAM (VERSIÓN MEJORADA) ======================
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
        // Timeout infinito → más estable para streaming continuo
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

        // ==================== CONVERSIÓN AUDIO (CAMBIO IMPORTANTE) ====================
        for (int i = 0; i < samples; i++) {
            int32_t raw = i2sBuffer[i];
            int16_t sample = (int16_t)(raw >> 10);     // ← Cambiado de >>13 a >>16 (prueba principal)

            if (abs(sample) > maxVolume) maxVolume = abs(sample);

            udpPacket[i*2]     = (uint8_t)(sample & 0xFF);
            udpPacket[i*2 + 1] = (uint8_t)(sample >> 8);
        }

        // ==================== HEADER + ENVÍO ====================
        AudioHeader header;
        header.sequence = sequence++;

        memcpy(packet, &header, sizeof(AudioHeader));
        memcpy(packet + sizeof(AudioHeader), udpPacket, samples * 2);

        udp.beginPacket(udpAddress, udpPort);
        udp.write(packet, sizeof(AudioHeader) + samples * 2);
        udp.endPacket();



        // UI
        drawVolumeBar(maxVolume);

        if (millis() - lastPrint > 800) {
            lastPrint = millis();
            char logMsg[100];
            snprintf(logMsg, sizeof(logMsg), "Sent seq=%lu  smp=%d  peak=%d", 
                     header.sequence, samples, maxVolume);
            drawLog(logMsg);
        }

        vTaskDelay(1);   // Deja respirar al WiFi
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
                vTaskDelay(300 / portTICK_PERIOD_MS);
                btnWifi.drawButton(false);
            }
            if (btnSend.contains(x, y)) {
                btnSend.drawButton(true);
                sendUDP("HELLO_FROM_ESP32 | t=" + String(millis()));
                vTaskDelay(300 / portTICK_PERIOD_MS);
                btnSend.drawButton(false);
            }
            if (btnMic.contains(x, y)) {
                btnMic.drawButton(true);
                vTaskDelay(50 / portTICK_PERIOD_MS);
                micStreaming = !micStreaming;

                if (micStreaming) {
                    btnMic.initButton(&tft, 120, 280, 160, 45, TFT_WHITE, TFT_GREEN, TFT_WHITE, (char*)"MIC ON ", 2);
                } else {
                    btnMic.initButton(&tft, 120, 280, 160, 45, TFT_WHITE, TFT_RED, TFT_WHITE, (char*)"MIC OFF", 2);
                }
                btnMic.drawButton(false);
                drawStatus();
                drawLog(micStreaming ? "Audio streaming STARTED" : "Audio streaming STOPPED");
                vTaskDelay(250 / portTICK_PERIOD_MS);
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
                udp.beginPacket(udpAddress, udpPort);
                udp.print("PING");
                udp.endPacket();
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
                    drawStatus();
                } else {
                    drawMessage(msg);
                }
            }

            if (millis() - lastPongTime > pongTimeout) {
                udpStatus = DISCONNECTED;
                drawStatus();
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// ====================== WIFI ======================
void connectWiFi() {
    Serial.println("Connecting WiFi...");
    WiFi.begin(ssid, password);

    // Desactivar Power Saving (clave para streaming continuo)
    esp_wifi_set_ps(WIFI_PS_NONE);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiStatus = WIFI_CONNECTED;
        udp.begin(udpPort);
        udpStatus = UDP_CONNECTED;
        Serial.println("WiFi OK - Power Saving DESACTIVADO");
        drawLog("WiFi OK - PS_NONE");
    } else {
        drawLog("WiFi FAILED");
    }
}

void sendUDP(String msg) {
    if (wifiStatus == WIFI_CONNECTED) {
        udp.beginPacket(udpAddress, udpPort);
        udp.print(msg);
        udp.endPacket();
        drawLog(("Sent: " + msg).c_str());
    }
}

void drawMessage(String msg) {
    xSemaphoreTake(screenMutex, portMAX_DELAY);
    tft.fillRect(15, 215, 210, 50, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(1);
    tft.setCursor(20, 220);
    tft.println(msg);
    xSemaphoreGive(screenMutex);
}

// ====================== SETUP ======================
void setup() {
    Serial.begin(115200);
    delay(1000);

    ts.begin();
    tft.init();
    tft.setRotation(0);

    screenMutex = xSemaphoreCreateMutex();

    drawUI();
    drawStatus();

    // Tareas
    xTaskCreatePinnedToCore(taskTouch,    "TouchTask", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(taskMicStream, "MicStream", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(taskUDP,       "UDPTask",   4096, NULL, 1, NULL, 0);

    Serial.println("ESP32 listo - Pulsa MIC para transmitir audio");
}

// ====================== LOOP ======================
void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}