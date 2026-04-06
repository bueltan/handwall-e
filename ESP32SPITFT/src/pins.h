#pragma once
#include <driver/i2s.h>

// ====================== PINOUT ======================
constexpr int I2S_WS  = 17;
constexpr int I2S_SCK = 4;
constexpr int I2S_SD  = 16;
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

constexpr int XPT2046_IRQ  = 36;
constexpr int XPT2046_MOSI = 32;
constexpr int XPT2046_MISO = 39;
constexpr int XPT2046_CLK  = 25;
constexpr int XPT2046_CS   = 33;