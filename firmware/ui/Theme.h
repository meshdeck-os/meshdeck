#pragma once
#include <stdint.h>

// RGB565 helpers
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// MeshDeck dark theme
#define C_BG          RGB565(10, 12, 16)     // near-black background
#define C_BG_ALT      RGB565(22, 26, 34)     // cards / list rows
#define C_BG_RAISED   RGB565(34, 40, 52)     // selected row / input bar
#define C_FG          RGB565(230, 234, 240)  // primary text
#define C_FG_DIM      RGB565(140, 148, 160)  // secondary text
#define C_FG_FAINT    RGB565(80, 86, 98)     // hairlines, disabled
#define C_ACCENT      RGB565(64, 156, 255)   // brand blue
#define C_ACCENT_DK   RGB565(28, 74, 130)
#define C_GREEN       RGB565(70, 200, 120)
#define C_YELLOW      RGB565(240, 200, 70)
#define C_ORANGE      RGB565(250, 150, 60)
#define C_RED         RGB565(240, 90, 90)
#define C_PURPLE      RGB565(180, 120, 240)
#define C_CYAN        RGB565(80, 210, 220)
#define C_PINK        RGB565(240, 120, 180)

// chat bubbles
#define C_BUB_IN      RGB565(38, 44, 58)     // incoming bubble
#define C_BUB_OUT     RGB565(30, 90, 160)    // outgoing bubble
#define C_BUB_OUT_TXT RGB565(235, 240, 248)

// map
#define C_MAP_BG      RGB565(8, 10, 14)
#define C_MAP_LAND    RGB565(60, 70, 88)     // coastline stroke
#define C_MAP_GRID    RGB565(24, 28, 38)
#define C_MAP_CITY    RGB565(120, 128, 142)
#define C_MAP_SELF    C_GREEN
#define C_MAP_NODE    C_CYAN
#define C_MAP_RPT     C_ORANGE

// terminal colors
#define C_TERM_TX     C_CYAN
#define C_TERM_RX     C_GREEN
#define C_TERM_SYS    C_FG_DIM
#define C_TERM_ERR    C_RED
#define C_TERM_IN     C_YELLOW

// status bar
#define STATUS_H      18
#define SCREEN_W      320
#define SCREEN_H      240

// palette for colour-coded usernames
static const uint16_t NAME_COLORS[8] = {
  C_CYAN, C_GREEN, C_YELLOW, C_PURPLE, C_PINK, C_ORANGE, RGB565(120, 180, 255), RGB565(160, 220, 160)
};
