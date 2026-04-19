#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

extern const char* row1Text[];
extern const char* row2Text[];
extern const char* row3Text[];
extern const char* row4Text[];

extern const char* row1Sym[];
extern const char* row2Sym[];
extern const char* row3Sym[];
extern const char* row4Sym[];

extern const int row1TextCount;
extern const int row2TextCount;
extern const int row3TextCount;
extern const int row4TextCount;

extern const int row1SymCount;
extern const int row2SymCount;
extern const int row3SymCount;
extern const int row4SymCount;

// ====================== SPECIAL BUTTON INDEXES ======================

constexpr int CAPS_INDEX  = 40;
constexpr int SPACE_INDEX = 41;
constexpr int DEL_INDEX   = 42;
constexpr int OK_INDEX    = 43;
constexpr int MODE_INDEX  = 44; // FN / ABC


// ====================== COLORS ======================

constexpr uint16_t UI_BG_COLOR      = TFT_BLACK;
constexpr uint16_t UI_BORDER_COLOR  = TFT_GREEN;
constexpr uint16_t UI_TEXT_COLOR    = TFT_GREEN;
constexpr uint16_t KEY_FILL_COLOR   = TFT_BLACK;
constexpr uint16_t KEY_TEXT_COLOR   = TFT_GREEN;
constexpr uint16_t KEY_BORDER_COLOR = TFT_GREEN;


