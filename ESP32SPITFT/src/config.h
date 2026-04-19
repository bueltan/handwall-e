#pragma once
#include <cstdint>

// ====================== NETWORK ======================
constexpr int UDP_PORT = 9999;

// ====================== UI ======================
constexpr int FONT_SIZE_S = 1;
constexpr unsigned long DEL_HOLD_MS = 1000;

// ====================== AUDIO ======================
constexpr int SAMPLE_RATE = 16000;
constexpr int SAMPLES_PER_PACKET = 160;
constexpr int BUFFER_LEN = SAMPLES_PER_PACKET;
constexpr int PACKET_SIZE = BUFFER_LEN * 2;

// ====================== NETWORK TIMERS ======================
constexpr unsigned long PING_INTERVAL = 2000;
constexpr unsigned long PONG_TIMEOUT  = 3000;

// ====================== INPUT ======================
constexpr uint8_t MAX_INPUT_LENGTH = 150;

// ====================== KEYBOARD ======================
constexpr int MAX_BUTTONS = 50;





