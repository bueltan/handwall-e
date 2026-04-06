#include "ui_config.h"

const char* row1Text[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
const char* row2Text[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
const char* row3Text[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "."};
const char* row4Text[] = {"Z", "X", "C", "V", "B", "N", "M"};

const char* row1Sym[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
const char* row2Sym[] = {"@", "#", "$", "%", "&", "/", "\\", ":", "_", "-"};
const char* row3Sym[] = {".", ",", ";", "!", "?", "\"", "'", "(", ")", "="};
const char* row4Sym[] = {"+", "*", "[", "]", "{", "}", "<", ">"};

const int row1TextCount = sizeof(row1Text) / sizeof(row1Text[0]);
const int row2TextCount = sizeof(row2Text) / sizeof(row2Text[0]);
const int row3TextCount = sizeof(row3Text) / sizeof(row3Text[0]);
const int row4TextCount = sizeof(row4Text) / sizeof(row4Text[0]);

const int row1SymCount = sizeof(row1Sym) / sizeof(row1Sym[0]);
const int row2SymCount = sizeof(row2Sym) / sizeof(row2Sym[0]);
const int row3SymCount = sizeof(row3Sym) / sizeof(row3Sym[0]);
const int row4SymCount = sizeof(row4Sym) / sizeof(row4Sym[0]);