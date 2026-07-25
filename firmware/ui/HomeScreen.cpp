#include "AllScreens.h"
#include "../MyMesh.h"
#include <RTClib.h>
#include <SPIFFS.h>

#define MENU_FILE  "/meshdeck_menu.bin"
#define MENU_MAGIC 0x314D444DUL   // "MDM1" - customizable home menu (#10)

struct AppDef { const char* label; ScreenId scr; uint16_t color; char glyph; bool disc; };
static const AppDef APPS[] = {
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
  { "WiFi",      SCR_WIFI,      C_ACCENT, 'W', false },
  { "Channels",  SCR_CHANNELS,  C_ORANGE, '#', false },
#ifdef MESHDECK_BETA
  { "Voice",     SCR_VOICE,     C_PINK,   'V', false },   // beta: audio PoC
#endif
};
#define N_APPS ((int)(sizeof(APPS) / sizeof(APPS[0])))

// grid layout: 5 rows x 3 cols
#define GRID_X0   14
#define GRID_Y0   98
#define GRID_ROWS 5
#define CELL_W    100
#define CELL_H    28

// open an app tile: Discover sends a flood advert first, everything else just navigates
static void openApp(UITask& ui, int i) {
  if (APPS[i].scr == SCR_SOS && ui.set.sos_disabled) {
    ui.toast("SOS is disabled (Settings)", C_YELLOW);
    return;
  }
  if (APPS[i].disc) ui.discover();
  else ui.go(APPS[i].scr);
}

void HomeScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  if (!_menuLoaded) loadMenu();

  // big clock
  char clk[8];
  ui.fmtClock(clk, sizeof(clk));
  c.setTextSize(5);
  c.setTextColor(C_FG);
  c.setCursor(84, 16);
  c.print(clk);

  // date + node name
  uint32_t e = ui.localEpoch();
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

  // combined GPS + WiFi status line, placed BELOW the clock/date/name so it
  // never overlaps the big clock digits
  {
    char g[36];
    uint16_t gcol;
    if (ui.prefs && ui.prefs->gps_enabled) {
      if (ui.gpsFix() && ui.sensors) {
        snprintf(g, sizeof(g), "GPS %.4f,%.4f", ui.sensors->node_lat, ui.sensors->node_lon);
        gcol = C_GREEN;
      } else {
        strcpy(g, "GPS searching");
        gcol = C_YELLOW;
      }
    } else {
      strcpy(g, "GPS off");
      gcol = C_FG_FAINT;
    }
    int ws = ui.wifiState();
    char w[24];
    uint16_t wcol;
    if (ws == 2)      { snprintf(w, sizeof(w), "WiFi %s", ui.wifiSsid()); wcol = C_GREEN; }
    else if (ws == 1) { strcpy(w, "WiFi..."); wcol = C_YELLOW; }
    else              { strcpy(w, "WiFi off"); wcol = C_FG_FAINT; }

    int total_w = (strlen(g) + 3 + strlen(w)) * 6;   // GPS + "   " + WiFi
    int x = SCREEN_W / 2 - total_w / 2;
    if (x < 6) x = 6;
    c.setCursor(x, 88);
    c.setTextColor(gcol);   c.print(g);
    c.setTextColor(C_FG_FAINT); c.print("   ");
    c.setTextColor(wcol);   c.print(w);
  }

  // app grid, built from the customizable order (#10). In edit mode every app
  // is shown (hidden ones dimmed); normally only visible apps, packed together.
  uint8_t slots[24];
  int nslots;
  if (_edit) { nslots = N_APPS; for (int s2 = 0; s2 < N_APPS; s2++) slots[s2] = _order[s2]; }
  else       { nslots = visible(slots); }

  for (int slot = 0; slot < nslots; slot++) {
    int a = slots[slot];
    int gx = GRID_X0 + (slot % 3) * CELL_W;
    int gy = GRID_Y0 + (slot / 3) * CELL_H;
    bool sel = slot == _sel;
    bool hid = _edit && _hidden[a];
    bool grabbed = _edit && _grab == slot;
    c.fillRoundRect(gx, gy, CELL_W - 8, CELL_H - 5, 6, sel ? C_BG_RAISED : C_BG_ALT);
    if (sel) c.drawRoundRect(gx, gy, CELL_W - 8, CELL_H - 5, 6, grabbed ? C_YELLOW : APPS[a].color);
    c.fillRoundRect(gx + 5, gy + 5, 20, 20, 5, hid ? C_FG_FAINT : APPS[a].color);
    c.setTextSize(2);
    c.setTextColor(C_BG);
    c.setCursor(gx + 9, gy + 8);
    c.write(APPS[a].glyph);
    c.setTextSize(1);
    c.setTextColor(hid ? C_FG_FAINT : (sel ? C_FG : C_FG_DIM));
    c.setCursor(gx + 30, gy + 11);
    c.print(APPS[a].label);
    if (!_edit && APPS[a].scr == SCR_CHAT) {
      int u = ui.store.totalUnread();
      if (u > 0) {
        c.fillCircle(gx + CELL_W - 16, gy + 8, 6, C_RED);
        c.setTextColor(0xFFFF);
        c.setCursor(gx + CELL_W - 16 - (u > 9 ? 6 : 3), gy + 5);
        c.print(u > 99 ? 99 : u);
      }
    }
  }

  if (_edit) {
    c.setTextColor(C_YELLOW);
    c.setCursor(6, SCREEN_H - 10);
    c.print(_grab >= 0 ? "move it, then click to drop" : "click=grab  H=hide/show  E=done");
  }
}

bool HomeScreen::key(uint8_t k) {
  if (_edit) {
    if (k == 'h' || k == 'H') { _hidden[_order[_sel]] = !_hidden[_order[_sel]]; return true; }
    if (k == 'e' || k == 'E' || k == 0x0D) {
      _edit = false; _grab = -1; saveMenu();
      uint8_t v[24]; int nv = visible(v); if (_sel >= nv) _sel = nv ? nv - 1 : 0;
      return true;
    }
    return true;   // swallow other keys while editing
  }
  if (k == 'e' || k == 'E') { _edit = true; _grab = -1; _sel = 0; return true; }
  uint8_t vis[24]; int nv = visible(vis);
  if (k >= '1' && k <= '9') { int idx = k - '1'; if (idx < nv) openApp(ui, vis[idx]); return true; }
  if (k == 0x0D) { if (_sel < nv) openApp(ui, vis[_sel]); return true; }
  return false;
}

bool HomeScreen::nav(NavEvent e) {
  if (_edit) {
    if (e == NAV_BACK)   { _edit = false; _grab = -1; saveMenu();
                           uint8_t v[24]; int nv = visible(v); if (_sel >= nv) _sel = nv ? nv - 1 : 0; return true; }
    if (e == NAV_SELECT) { _grab = (_grab == _sel) ? -1 : _sel; return true; }
    int cur = _sel, dst = cur, n = N_APPS;
    switch (e) {
      case NAV_UP:    if (cur >= 3) dst = cur - 3; break;
      case NAV_DOWN:  if (cur + 3 < n) dst = cur + 3; break;
      case NAV_LEFT:  if (cur % 3) dst = cur - 1; break;
      case NAV_RIGHT: if (cur % 3 < 2 && cur + 1 < n) dst = cur + 1; break;
      default: return true;
    }
    if (dst != cur) {
      if (_grab == cur) { uint8_t t = _order[cur]; _order[cur] = _order[dst]; _order[dst] = t; _grab = dst; }
      _sel = dst;
    }
    return true;
  }
  uint8_t vis[24]; int nv = visible(vis);
  switch (e) {
    case NAV_UP:    if (_sel >= 3) _sel -= 3; return true;
    case NAV_DOWN:  if (_sel + 3 < nv) _sel += 3; return true;
    case NAV_LEFT:  if (_sel % 3) _sel--; return true;
    case NAV_RIGHT: if (_sel % 3 < 2 && _sel + 1 < nv) _sel++; return true;
    case NAV_SELECT: if (_sel < nv) openApp(ui, vis[_sel]); return true;
    case NAV_BACK:  return true;   // already home
    default: return false;
  }
}

bool HomeScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  if (e.y < GRID_Y0) return true;
  int col = (e.x - GRID_X0) / CELL_W;
  int row = (e.y - GRID_Y0) / CELL_H;
  int slot = row * 3 + col;
  if (col < 0 || col >= 3 || row < 0 || row >= GRID_ROWS) return true;
  if (_edit) { if (slot < N_APPS) _sel = slot; return true; }
  uint8_t vis[24]; int nv = visible(vis);
  if (slot >= 0 && slot < nv) { _sel = slot; openApp(ui, vis[slot]); }
  return true;
}

void HomeScreen::enter() {
  if (!_menuLoaded) loadMenu();
  _edit = false; _grab = -1;
  uint8_t v[24]; int nv = visible(v); if (_sel >= nv) _sel = nv ? nv - 1 : 0;
}

int HomeScreen::visible(uint8_t* out) {
  int n = 0;
  for (int s = 0; s < N_APPS; s++) { int a = _order[s]; if (!_hidden[a]) out[n++] = a; }
  return n;
}

void HomeScreen::loadMenu() {
  _menuLoaded = true;
  for (int i = 0; i < N_APPS; i++) { _order[i] = (uint8_t)i; _hidden[i] = false; }
  File f = SPIFFS.open(MENU_FILE, "r");
  if (f) {
    uint32_t magic = 0; uint8_t cnt = 0;
    if (f.read((uint8_t*)&magic, 4) == 4 && magic == MENU_MAGIC &&
        f.read(&cnt, 1) == 1 && cnt == N_APPS) {
      uint8_t ord[24], hid[24];
      if (f.read(ord, cnt) == cnt && f.read(hid, cnt) == cnt) {
        bool seen[24] = { false }; bool ok = true;
        for (int i = 0; i < cnt; i++) { if (ord[i] >= N_APPS || seen[ord[i]]) { ok = false; break; } seen[ord[i]] = true; }
        if (ok) for (int i = 0; i < cnt; i++) { _order[i] = ord[i]; _hidden[i] = hid[i] ? true : false; }
      }
    }
    f.close();
  }
}

void HomeScreen::saveMenu() {
  File f = SPIFFS.open(MENU_FILE, "w");
  if (!f) return;
  uint32_t magic = MENU_MAGIC; uint8_t cnt = N_APPS, hid[24];
  for (int i = 0; i < N_APPS; i++) hid[i] = _hidden[i] ? 1 : 0;
  f.write((uint8_t*)&magic, 4); f.write(&cnt, 1);
  f.write(_order, N_APPS); f.write(hid, N_APPS);
  f.close();
}
