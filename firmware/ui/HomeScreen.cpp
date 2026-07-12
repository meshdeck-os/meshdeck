#include "AllScreens.h"
#include "../MyMesh.h"
#include <RTClib.h>

struct AppDef { const char* label; ScreenId scr; uint16_t color; char glyph; bool disc; };
#define N_APPS 12
static const AppDef APPS[N_APPS] = {
  { "Chat",      SCR_CHAT,      C_ACCENT, 'C', false },
  { "Contacts",  SCR_CONTACTS,  C_GREEN,  '@', false },
  { "Discover",  SCR_LASTHEARD, C_CYAN,   '*', true  },   // advert + jump to Heard
  { "Heard",     SCR_LASTHEARD, C_YELLOW, 'H', false },
  { "Repeaters", SCR_REPEATERS, C_ORANGE, 'R', false },
  { "Map",       SCR_MAP,       C_CYAN,   'M', false },
  { "Radio",     SCR_DIAG,      C_GREEN,  'i', false },   // diagnostics
  { "Trace",     SCR_TRACE,     C_PURPLE, 'T', false },
  { "Noise",     SCR_NOISE,     C_PINK,   'N', false },
  { "Terminal",  SCR_TERMINAL,  C_FG,     '>', false },
  { "SOS",       SCR_SOS,       C_RED,    '!', false },
  { "Settings",  SCR_SETTINGS,  C_FG_DIM, 'S', false },
};

// grid layout: 4 rows x 3 cols
#define GRID_X0   14
#define GRID_Y0   94
#define GRID_ROWS 4
#define CELL_W    100
#define CELL_H    35

// open an app tile: Discover sends a flood advert first, everything else just navigates
static void openApp(UITask& ui, int i) {
  if (APPS[i].disc) ui.discover();
  else ui.go(APPS[i].scr);
}

void HomeScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);

  // big clock
  char clk[8];
  ui.fmtClock(clk, sizeof(clk));
  c.setTextSize(5);
  c.setTextColor(C_FG);
  c.setCursor(84, 16);
  c.print(clk);

  // date + node name
  uint32_t e = ui.epochNow();
  c.setTextSize(1);
  c.setTextColor(C_FG_DIM);
  if (e > 1000000000) {
    DateTime dt(e);
    static const char* DOW[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char d[40];
    snprintf(d, sizeof(d), "%s %02d/%02d/%04d", DOW[dt.dayOfTheWeek()], dt.day(), dt.month(), dt.year());
    c.setCursor(SCREEN_W / 2 - strlen(d) * 3, 62);
    c.print(d);
  }
  char nn[48];
  snprintf(nn, sizeof(nn), "%s  |  %d unread", ui.prefs ? ui.prefs->node_name : "?", ui.store.totalUnread());
  c.setCursor(SCREEN_W / 2 - strlen(nn) * 3, 76);
  c.setTextColor(C_FG_FAINT);
  c.print(nn);

  // battery + mesh in corners
  char b[16];
  int pct = ui.batteryPercent();
  if (pct >= 0) {
    snprintf(b, sizeof(b), "%d%%", pct);
    c.setTextColor(pct > 20 ? C_FG_DIM : C_RED);
    c.setCursor(SCREEN_W - 8 - strlen(b) * 6, 8);
    c.print(b);
  }
  int bars = ui.meshBars();
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 3;
    c.fillRect(8 + i * 5, 20 - bh, 3, bh, i < bars ? C_ACCENT : C_FG_FAINT);
  }
  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, 26);
  c.print("mesh");

  // app grid 4x3
  for (int i = 0; i < N_APPS; i++) {
    int gx = GRID_X0 + (i % 3) * CELL_W;
    int gy = GRID_Y0 + (i / 3) * CELL_H;
    bool sel = i == _sel;
    c.fillRoundRect(gx, gy, CELL_W - 8, CELL_H - 5, 6, sel ? C_BG_RAISED : C_BG_ALT);
    if (sel) c.drawRoundRect(gx, gy, CELL_W - 8, CELL_H - 5, 6, APPS[i].color);
    // glyph badge
    c.fillRoundRect(gx + 5, gy + 5, 20, 20, 5, APPS[i].color);
    c.setTextSize(2);
    c.setTextColor(C_BG);
    c.setCursor(gx + 9, gy + 8);
    c.write(APPS[i].glyph);
    c.setTextSize(1);
    c.setTextColor(sel ? C_FG : C_FG_DIM);
    c.setCursor(gx + 30, gy + 11);
    c.print(APPS[i].label);
    // unread badge on chat
    if (APPS[i].scr == SCR_CHAT) {
      int u = ui.store.totalUnread();
      if (u > 0) {
        c.fillCircle(gx + CELL_W - 16, gy + 8, 6, C_RED);
        c.setTextColor(0xFFFF);
        c.setCursor(gx + CELL_W - 16 - (u > 9 ? 6 : 3), gy + 5);
        c.print(u > 99 ? 99 : u);
      }
    }
  }
}

bool HomeScreen::key(uint8_t k) {
  if (k >= '1' && k <= '9') {
    openApp(ui, k - '1');
    return true;
  }
  if (k == 0x0D) { openApp(ui, _sel); return true; }
  return false;
}

bool HomeScreen::nav(NavEvent e) {
  switch (e) {
    case NAV_UP:    if (_sel >= 3) _sel -= 3; return true;
    case NAV_DOWN:  if (_sel < N_APPS - 3) _sel += 3; return true;
    case NAV_LEFT:  if (_sel % 3) _sel--; return true;
    case NAV_RIGHT: if (_sel % 3 < 2 && _sel + 1 < N_APPS) _sel++; return true;
    case NAV_SELECT: openApp(ui, _sel); return true;
    case NAV_BACK:
      // already at home - do nothing (never blank the screen here)
      return true;
    default: return false;
  }
}

bool HomeScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  if (e.y < GRID_Y0) return true;
  int col = (e.x - GRID_X0) / CELL_W;
  int row = (e.y - GRID_Y0) / CELL_H;
  int idx = row * 3 + col;
  if (col >= 0 && col < 3 && row >= 0 && row < GRID_ROWS && idx < N_APPS) {
    _sel = idx;
    openApp(ui, _sel);
  }
  return true;
}
