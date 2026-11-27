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
  { "NewMaps",   SCR_NEWMAPS,   C_GREEN,  'N', false },   // OSM multi-layer vector
  { "Radio",     SCR_DIAG,      C_GREEN,  'i', false },   // diagnostics
  { "Trace",     SCR_TRACE,     C_PURPLE, 'T', false },
  { "Noise",     SCR_NOISE,     C_PINK,   '~', false },
  { "Terminal",  SCR_TERMINAL,  C_FG,     '>', false },
  { "SOS",       SCR_SOS,       C_RED,    '!', false },
  { "Settings",  SCR_SETTINGS,  C_FG_DIM, 'S', false },
  { "WiFi",      SCR_WIFI,      C_ACCENT, 'W', false },
  { "Channels",  SCR_CHANNELS,  C_ORANGE, '#', false },
};
#define N_APPS ((int)(sizeof(APPS) / sizeof(APPS[0])))

// grid layout: 5 rows x 3 cols = up to 15 apps on Home; the rest live in All apps
// (#28). APPS[] is persisted by index: only ever APPEND new apps at the end.
// GRID_Y0 must sit below GPS/WiFi line (baseline ~86 + 8px font ~ 94)
#define GRID_X0   14
#define GRID_Y0   100
#define GRID_ROWS 5
#define CELL_W    100
#define CELL_H    27   // 100 + 5*27 = 235 < 240
#define HOME_SLOTS (GRID_ROWS * 3)
#define MENU_MAX  24   // capacity of the saved layout arrays
static_assert(N_APPS <= MENU_MAX, "grow MENU_MAX");

// open an app tile: Discover sends a flood advert first, everything else just navigates
static void openApp(UITask& ui, int i) {
  if (APPS[i].scr == SCR_SOS && ui.set.sos_disabled) {
    ui.toast("SOS is disabled (Settings)", C_YELLOW);
    return;
  }
  if (APPS[i].disc) ui.discover();
  else ui.go(APPS[i].scr);
}

// ---------------------------------------------------------------- shared menu state
// g_order is a permutation of app indices; Home shows the apps whose g_off flag
// is clear, in g_order order. Saved as: magic, count, order[count], off[count]
// (off[] indexed by app). Same file format as #10, so existing layouts carry over.

static uint8_t g_order[MENU_MAX];
static bool    g_off[MENU_MAX];     // true = not on Home (only in All apps)
static bool    g_loaded = false;

static int favs(uint8_t* out) {
  int n = 0;
  for (int s = 0; s < N_APPS; s++) { int a = g_order[s]; if (!g_off[a]) out[n++] = a; }
  return n;
}

static void menuSave() {
  File f = SPIFFS.open(MENU_FILE, "w");
  if (!f) return;
  uint32_t magic = MENU_MAGIC; uint8_t cnt = N_APPS, off[MENU_MAX];
  for (int i = 0; i < N_APPS; i++) off[i] = g_off[i] ? 1 : 0;
  f.write((uint8_t*)&magic, 4); f.write(&cnt, 1);
  f.write(g_order, N_APPS); f.write(off, N_APPS);
  f.close();
}

// Load the saved layout. A file from an older build may list fewer apps: keep
// its order and flags and append the apps it doesn't know about. (Before #28
// a count mismatch reset the whole layout.)
static void menuLoad() {
  g_loaded = true;
  int n = 0;
  bool placed[MENU_MAX] = { false };
  File f = SPIFFS.open(MENU_FILE, "r");
  if (f) {
    uint32_t magic = 0; uint8_t cnt = 0;
    uint8_t ord[MENU_MAX], off[MENU_MAX];
    if (f.read((uint8_t*)&magic, 4) == 4 && magic == MENU_MAGIC &&
        f.read(&cnt, 1) == 1 && cnt >= 1 && cnt <= N_APPS &&
        f.read(ord, cnt) == cnt && f.read(off, cnt) == cnt) {
      bool ok = true;                     // must be a permutation of 0..cnt-1
      for (int i = 0; i < cnt && ok; i++) {
        if (ord[i] >= cnt || placed[ord[i]]) ok = false;
        else placed[ord[i]] = true;
      }
      if (ok) {
        for (int i = 0; i < cnt; i++) { g_order[n++] = ord[i]; g_off[i] = off[i] != 0; }
      } else {
        memset(placed, 0, sizeof(placed));
      }
    }
    f.close();
  }
  // Apps the file doesn't list: on a fresh install the first HOME_SLOTS go on
  // Home; after an upgrade, newly added apps start in All apps only, so a
  // layout the user arranged is never changed behind their back.
  const bool fresh = n == 0;
  int on = 0;
  for (int a = 0; a < N_APPS; a++) {
    if (placed[a]) continue;
    g_order[n++] = (uint8_t)a;
    g_off[a] = !fresh || on >= HOME_SLOTS;
    if (!g_off[a]) on++;
  }
}

// rebuild the order as: favourites (in the given order), then everything else
static void setFavs(const uint8_t* fav, int nf) {
  uint8_t ord[MENU_MAX]; bool in[MENU_MAX] = { false };
  int n = 0;
  for (int i = 0; i < nf; i++) { ord[n++] = fav[i]; in[fav[i]] = true; }
  for (int s = 0; s < N_APPS; s++) if (!in[g_order[s]]) ord[n++] = g_order[s];
  for (int a = 0; a < N_APPS; a++) g_off[a] = !in[a];
  memcpy(g_order, ord, N_APPS);
}

static void moveFav(int from, int to) {
  uint8_t f[MENU_MAX]; int n = favs(f);
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
  uint8_t a = f[from];
  if (from < to) memmove(&f[from], &f[from + 1], to - from);
  else           memmove(&f[to + 1], &f[to], from - to);
  f[to] = a;
  setFavs(f, n);
}

// add to the end of Home, or remove. false = Home is full.
static bool toggleHome(int app) {
  uint8_t f[MENU_MAX]; int n = favs(f);
  if (!g_off[app]) { g_off[app] = true; menuSave(); return true; }
  if (n >= HOME_SLOTS) return false;
  f[n++] = (uint8_t)app;
  setFavs(f, n);
  menuSave();
  return true;
}

// ---------------------------------------------------------------- Home layout

// buttons beside the clock (normal mode)
#define BTN_Y      40
#define BTN_H      18
#define EDITB_X    8
#define EDITB_W    48
#define APPSB_W    56
#define APPSB_X    (SCREEN_W - APPSB_W - 8)
// edit-mode toolbar
#define TB_Y       62
#define TB_H       26
#define TB_W       96
#define TB_REM_X   8
#define TB_APPS_X  ((SCREEN_W - TB_W) / 2)
#define TB_DONE_X  (SCREEN_W - TB_W - 8)

static bool inBox(int x, int y, int bx, int by, int bw, int bh, int pad = 4) {
  return x >= bx - pad && x < bx + bw + pad && y >= by - pad && y < by + bh + pad;
}

// grid slot under a screen point, or -1
static int slotAt(int x, int y) {
  if (x < GRID_X0 || y < GRID_Y0) return -1;
  int col = (x - GRID_X0) / CELL_W, row = (y - GRID_Y0) / CELL_H;
  if (col >= 3 || row >= GRID_ROWS) return -1;
  return row * 3 + col;
}

static void button(GFXcanvas16& c, int x, int y, int w, int h, const char* label,
                   bool focus, uint16_t col, bool enabled = true) {
  c.fillRoundRect(x, y, w, h, 4, focus ? C_BG_RAISED : C_BG_ALT);
  c.drawRoundRect(x, y, w, h, 4, focus ? col : C_FG_FAINT);
  c.setTextSize(1);
  c.setTextColor(!enabled ? C_FG_FAINT : (focus ? C_FG : C_FG_DIM));
  c.setCursor(x + (w - (int)strlen(label) * 6) / 2, y + (h - 8) / 2);
  c.print(label);
}

void HomeScreen::clampSel() {
  uint8_t f[MENU_MAX]; int n = favs(f);
  if (_edit) { if (_sel < 0) _sel = 0; if (_sel >= n) _sel = n ? n - 1 : 0; }
  else if (_sel >= n) _sel = n ? n - 1 : -1;
}

void HomeScreen::startEdit(int grab) {
  _edit = true; _drag = false;
  _sel = grab >= 0 ? grab : 0;
  _grab = grab;
  clampSel();
}

void HomeScreen::endEdit() {
  _edit = false; _grab = -1; _drag = false;
  menuSave();
  clampSel();
}

void HomeScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  if (!g_loaded) menuLoad();
  uint8_t f[MENU_MAX];
  int nf = favs(f);

  if (_edit) {
    // edit toolbar replaces the clock area
    c.setTextSize(2);
    c.setTextColor(C_YELLOW);
    c.setCursor(10, 10);
    c.print("Edit Home");
    c.setTextSize(1);
    char cnt[12];
    snprintf(cnt, sizeof(cnt), "%d/%d", nf, HOME_SLOTS);
    c.setTextColor(C_FG_FAINT);
    c.setCursor(SCREEN_W - 8 - strlen(cnt) * 6, 14);
    c.print(cnt);
    c.setTextColor(C_FG_DIM);
    c.setCursor(10, 34);
    c.print(_grab >= 0 ? (_drag ? "drag to the new spot, then lift"
                                : "move it, then click to drop")
                       : "hold+drag a tile, or click to grab it");
    c.setTextColor(C_FG_FAINT);
    c.setCursor(10, 46);
    c.print("H=remove  A=all apps  E=done");
    button(c, TB_REM_X,  TB_Y, TB_W, TB_H, "Remove",   false, C_RED, nf > 0);
    button(c, TB_APPS_X, TB_Y, TB_W, TB_H, "All apps", false, C_ACCENT);
    button(c, TB_DONE_X, TB_Y, TB_W, TB_H, "Done",     false, C_GREEN);
  } else {
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
    if (ui.batteryCharging()) {
      // level isn't measurable through the charger - say so instead of "100%"
      snprintf(b, sizeof(b), "CHG");
      c.setTextColor(C_YELLOW);
      c.setCursor(SCREEN_W - 8 - strlen(b) * 6, 8);
      c.print(b);
    } else if (pct >= 0) {
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
      // Keep above app grid (GRID_Y0); leave a few px of air
      c.setCursor(x, 86);
      c.setTextColor(gcol);   c.print(g);
      c.setTextColor(C_FG_FAINT); c.print("   ");
      c.setTextColor(wcol);   c.print(w);
    }

    // Edit / All apps buttons either side of the clock (trackball: up from the top row)
    button(c, EDITB_X, BTN_Y, EDITB_W, BTN_H, "Edit", _sel == -2, C_YELLOW);
    button(c, APPSB_X, BTN_Y, APPSB_W, BTN_H, "Apps", _sel == -1, C_ACCENT);
  }

  // favourites grid (#10, #28)
  for (int slot = 0; slot < nf; slot++) {
    int a = f[slot];
    int gx = GRID_X0 + (slot % 3) * CELL_W;
    int gy = GRID_Y0 + (slot / 3) * CELL_H;
    bool sel = slot == _sel;
    bool grabbed = _edit && _grab == slot;
    const int tile_h = CELL_H - 4;
    c.fillRoundRect(gx, gy, CELL_W - 8, tile_h, 5, sel ? C_BG_RAISED : C_BG_ALT);
    if (sel) c.drawRoundRect(gx, gy, CELL_W - 8, tile_h, 5, grabbed ? C_YELLOW : APPS[a].color);
    // glyph badge (slightly smaller so CELL_H=27 still looks balanced)
    c.fillRoundRect(gx + 5, gy + 3, 18, 18, 4, APPS[a].color);
    c.setTextSize(2);
    c.setTextColor(C_BG);
    c.setCursor(gx + 8, gy + 5);
    c.write(APPS[a].glyph);
    c.setTextSize(1);
    c.setTextColor(sel ? C_FG : C_FG_DIM);
    c.setCursor(gx + 28, gy + 9);
    c.print(APPS[a].label);
    // unread badge on chat
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
  if (nf == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(GRID_X0, GRID_Y0 + 8);
    c.print(_edit ? "Home is empty - add apps from All apps"
                  : "No apps on Home - open Apps to add some");
  }
}

bool HomeScreen::key(uint8_t k) {
  uint8_t f[MENU_MAX];
  int nf = favs(f);
  if (_edit) {
    if ((k == 'h' || k == 'H') && _sel < nf) { g_off[f[_sel]] = true; _grab = -1; clampSel(); return true; }
    if (k == 'a' || k == 'A') { endEdit(); ui.go(SCR_APPS); return true; }
    if (k == 'e' || k == 'E' || k == 0x0D) { endEdit(); return true; }
    return true;   // swallow other keys while editing
  }
  if (k == 'e' || k == 'E') { startEdit(-1); return true; }
  if (k == 'a' || k == 'A') { ui.go(SCR_APPS); return true; }
  if (k >= '1' && k <= '9') { int idx = k - '1'; if (idx < nf) openApp(ui, f[idx]); return true; }
  if (k == 0x0D) {
    if (_sel == -1) ui.go(SCR_APPS);
    else if (_sel == -2) startEdit(-1);
    else if (_sel < nf) openApp(ui, f[_sel]);
    return true;
  }
  return false;
}

bool HomeScreen::nav(NavEvent e) {
  uint8_t f[MENU_MAX];
  int n = favs(f);
  if (_edit) {
    if (e == NAV_BACK)   { endEdit(); return true; }
    if (e == NAV_SELECT) { if (n) _grab = (_grab == _sel) ? -1 : _sel; return true; }
    int cur = _sel, dst = cur;
    switch (e) {
      case NAV_UP:    if (cur >= 3) dst = cur - 3; break;
      case NAV_DOWN:  if (cur + 3 < n) dst = cur + 3; break;
      case NAV_LEFT:  if (cur % 3) dst = cur - 1; break;
      case NAV_RIGHT: if (cur % 3 < 2 && cur + 1 < n) dst = cur + 1; break;
      default: return true;
    }
    if (dst != cur) {
      if (_grab == cur) { moveFav(cur, dst); _grab = dst; }
      _sel = dst;
    }
    return true;
  }
  // _sel -2 = Edit button, -1 = Apps button, 0.. = grid
  if (_sel < 0) {
    switch (e) {
      case NAV_LEFT:   _sel = -2; return true;
      case NAV_RIGHT:  _sel = -1; return true;
      case NAV_DOWN:   if (n) _sel = 0; return true;
      case NAV_UP:     return true;
      case NAV_SELECT: if (_sel == -1) ui.go(SCR_APPS); else startEdit(-1); return true;
      case NAV_BACK:   return true;
      default: return false;
    }
  }
  switch (e) {
    case NAV_UP:    if (_sel >= 3) _sel -= 3; else _sel = (_sel == 0) ? -2 : -1; return true;
    case NAV_DOWN:  if (_sel + 3 < n) _sel += 3; return true;
    case NAV_LEFT:  if (_sel % 3) _sel--; return true;
    case NAV_RIGHT: if (_sel % 3 < 2 && _sel + 1 < n) _sel++; return true;
    case NAV_SELECT: if (_sel < n) openApp(ui, f[_sel]); return true;
    case NAV_BACK:  return true;   // already home
    default: return false;
  }
}

bool HomeScreen::touch(const TouchEvent& e) {
  uint8_t f[MENU_MAX];
  int n = favs(f);
  int slot = slotAt(e.x, e.y);
  if (_edit) {
    // touch-move: long-press grabbed a tile; follow the finger until it lifts
    if (_drag) {
      if (e.kind == TouchEvent::DRAG) {
        if (slot >= 0 && slot < n && slot != _grab) { moveFav(_grab, slot); _grab = _sel = slot; }
      } else if (e.kind == TouchEvent::RELEASE || e.kind == TouchEvent::TAP) {
        _drag = false; _grab = -1; menuSave();
      }
      return true;
    }
    if (e.kind == TouchEvent::LONG && slot >= 0 && slot < n) { _sel = _grab = slot; _drag = true; return true; }
    if (e.kind == TouchEvent::TAP) {
      if (inBox(e.x, e.y, TB_REM_X, TB_Y, TB_W, TB_H)) {
        if (_sel < n) { g_off[f[_sel]] = true; _grab = -1; clampSel(); }
      } else if (inBox(e.x, e.y, TB_APPS_X, TB_Y, TB_W, TB_H)) {
        endEdit(); ui.go(SCR_APPS);
      } else if (inBox(e.x, e.y, TB_DONE_X, TB_Y, TB_W, TB_H)) {
        endEdit();
      } else if (slot >= 0 && slot < n) {
        _sel = slot; _grab = -1;
      }
    }
    return true;   // no swipe-back while editing: use Done
  }
  if (e.kind == TouchEvent::LONG && slot >= 0 && slot < n) { startEdit(slot); _drag = true; return true; }
  if (e.kind != TouchEvent::TAP) return false;
  if (inBox(e.x, e.y, APPSB_X, BTN_Y, APPSB_W, BTN_H)) { _sel = -1; ui.go(SCR_APPS); return true; }
  if (inBox(e.x, e.y, EDITB_X, BTN_Y, EDITB_W, BTN_H)) { startEdit(-1); return true; }
  if (slot >= 0 && slot < n) { _sel = slot; openApp(ui, f[slot]); }
  return true;
}

void HomeScreen::enter() {
  if (!g_loaded) menuLoad();
  _edit = false; _grab = -1; _drag = false;
  clampSel();
}

// ---------------------------------------------------------------- All apps (#28)

#define AL_Y      (STATUS_H + 3)
#define AL_RH     22
#define AL_ROWS   ((SCREEN_H - AL_Y - 14) / AL_RH)
#define AL_TOG_W  76
#define AL_TOG_X  (SCREEN_W - AL_TOG_W - 10)

static uint8_t g_alpha[MENU_MAX];   // app indices sorted by label
static bool    g_alpha_ok = false;

static void buildAlpha() {
  for (int i = 0; i < N_APPS; i++) g_alpha[i] = (uint8_t)i;
  for (int i = 1; i < N_APPS; i++)          // insertion sort: tiny list
    for (int j = i; j > 0 && strcasecmp(APPS[g_alpha[j]].label, APPS[g_alpha[j - 1]].label) < 0; j--) {
      uint8_t t = g_alpha[j]; g_alpha[j] = g_alpha[j - 1]; g_alpha[j - 1] = t;
    }
  g_alpha_ok = true;
}

void AppsScreen::enter() {
  if (!g_loaded) menuLoad();
  if (!g_alpha_ok) buildAlpha();
  if (_sel >= N_APPS) _sel = N_APPS - 1;
  _drag_acc = 0;
  scrollTo(_sel);
}

void AppsScreen::scrollTo(int row) {
  if (row < _top) _top = row;
  if (row >= _top + AL_ROWS) _top = row - AL_ROWS + 1;
  int maxTop = N_APPS > AL_ROWS ? N_APPS - AL_ROWS : 0;
  if (_top > maxTop) _top = maxTop;
  if (_top < 0) _top = 0;
}

void AppsScreen::toggle(int row) {
  int a = g_alpha[row];
  bool adding = g_off[a];
  if (!toggleHome(a)) {
    char m[40];
    snprintf(m, sizeof(m), "Home is full (%d) - remove one", HOME_SLOTS);
    ui.toast(m, C_YELLOW);
    return;
  }
  ui.toast(adding ? "Added to Home" : "Removed from Home", adding ? C_GREEN : C_FG_DIM);
}

void AppsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("All apps");
  c.setTextSize(1);
  for (int r = 0; r < AL_ROWS && _top + r < N_APPS; r++) {
    int row = _top + r, a = g_alpha[row];
    int y = AL_Y + r * AL_RH;
    bool sel = row == _sel, home = !g_off[a];
    c.fillRoundRect(6, y, SCREEN_W - 12, AL_RH - 3, 4, sel ? C_BG_RAISED : C_BG);
    if (sel) c.drawRoundRect(6, y, SCREEN_W - 12, AL_RH - 3, 4, APPS[a].color);
    c.fillRoundRect(12, y + 1, 17, 17, 4, APPS[a].color);
    c.setTextSize(2);
    c.setTextColor(C_BG);
    c.setCursor(15, y + 2);
    c.write(APPS[a].glyph);
    c.setTextSize(1);
    c.setTextColor(sel ? C_FG : C_FG_DIM);
    c.setCursor(38, y + 6);
    c.print(APPS[a].label);
    // on-Home toggle
    int ty = y + 2, th = AL_RH - 7;
    if (home) {
      c.fillRoundRect(AL_TOG_X, ty, AL_TOG_W, th, 3, C_BG_ALT);
      c.drawRoundRect(AL_TOG_X, ty, AL_TOG_W, th, 3, C_ACCENT);
      c.setTextColor(C_ACCENT);
      c.setCursor(AL_TOG_X + (AL_TOG_W - 7 * 6) / 2, ty + (th - 8) / 2);
      c.print("On Home");
    } else {
      c.drawRoundRect(AL_TOG_X, ty, AL_TOG_W, th, 3, C_FG_FAINT);
      c.setTextColor(C_FG_FAINT);
      c.setCursor(AL_TOG_X + (AL_TOG_W - 6 * 6) / 2, ty + (th - 8) / 2);
      c.print("+ Home");
    }
  }
  // scroll indicator
  if (N_APPS > AL_ROWS) {
    int trackH = AL_ROWS * AL_RH - 3;
    int barH = trackH * AL_ROWS / N_APPS;
    int barY = AL_Y + (trackH - barH) * _top / (N_APPS - AL_ROWS);
    c.fillRect(SCREEN_W - 4, barY, 2, barH, C_FG_FAINT);
  }
  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, SCREEN_H - 11);
  c.print("enter=open  h=add/remove Home  back");
}

bool AppsScreen::key(uint8_t k) {
  if (k == 0x0D) { openApp(ui, g_alpha[_sel]); return true; }
  if (k == 'h' || k == 'H') { toggle(_sel); return true; }
  return false;                 // i/k/space fall back to nav; backspace = back
}

bool AppsScreen::nav(NavEvent e) {
  switch (e) {
    case NAV_UP:     if (_sel > 0) _sel--; scrollTo(_sel); return true;
    case NAV_DOWN:   if (_sel < N_APPS - 1) _sel++; scrollTo(_sel); return true;
    case NAV_RIGHT:  toggle(_sel); return true;
    case NAV_SELECT: openApp(ui, g_alpha[_sel]); return true;
    default:         return false;   // BACK / LEFT bubble up
  }
}

bool AppsScreen::touch(const TouchEvent& e) {
  if (e.kind == TouchEvent::DRAG) {
    if (N_APPS <= AL_ROWS) return false;
    _drag_acc += e.dy;
    while (_drag_acc <= -AL_RH) { _drag_acc += AL_RH; if (_top < N_APPS - AL_ROWS) _top++; }
    while (_drag_acc >=  AL_RH) { _drag_acc -= AL_RH; if (_top > 0) _top--; }
    if (_sel < _top) _sel = _top;
    if (_sel >= _top + AL_ROWS) _sel = _top + AL_ROWS - 1;
    return false;               // let a long horizontal swipe still go back
  }
  if (e.kind == TouchEvent::RELEASE) { _drag_acc = 0; return false; }
  if (e.y < AL_Y) return false;
  int row = _top + (e.y - AL_Y) / AL_RH;
  if (row < 0 || row >= N_APPS || row >= _top + AL_ROWS) return false;
  if (e.kind == TouchEvent::LONG) { _sel = row; toggle(row); return true; }
  if (e.kind != TouchEvent::TAP) return false;
  _sel = row;
  if (e.x >= AL_TOG_X - 6) toggle(row);
  else openApp(ui, g_alpha[row]);
  return true;
}
