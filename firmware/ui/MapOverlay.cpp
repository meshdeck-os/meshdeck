#include "MapOverlay.h"
#include "../MyMesh.h"
#include <math.h>

// ---- layout (draw and hit-testing must agree) ----
static const char* const ITEMS[] = { "GPS info", "Center on me", "Set my position here" };
static const int N_ITEMS = sizeof(ITEMS) / sizeof(ITEMS[0]);

#define MENU_RH  26
#define MENU_W   240
#define MENU_X   ((SCREEN_W - MENU_W) / 2)
#define MENU_Y   (STATUS_H + 30)
#define MENU_H   (N_ITEMS * MENU_RH + 16)

#define CHIP_W   42
#define CHIP_H   16
#define CHIP_X   (SCREEN_W - CHIP_W - 6)
#define CHIP_Y   (STATUS_H + 4)

#define BAN_X    6
#define BAN_W    (SCREEN_W - 12)
#define BAN_H    62
#define BAN_Y    (SCREEN_H - BAN_H - 6)
#define BTN_W    54
#define BTN_H    24
#define BTN_Y    (BAN_Y + 19)
#define SET_X    (BAN_X + BAN_W - 2 * BTN_W - 12)
#define CAN_X    (BAN_X + BAN_W - BTN_W - 6)

static bool inRect(int x, int y, int rx, int ry, int rw, int rh, int pad = 0) {
  return x >= rx - pad && x < rx + rw + pad && y >= ry - pad && y < ry + rh + pad;
}

void MapOverlay::fmtLatLon(char* out, size_t n, double lat, double lon) {
  snprintf(out, n, "%.5f %c  %.5f %c", fabs(lat), lat >= 0 ? 'N' : 'S',
           fabs(lon), lon >= 0 ? 'E' : 'W');
}

// ---------------------------------------------------------------- drawing

void MapOverlay::drawMenu(UITask& ui) {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);                 // opaque: never leave map pixels underneath
  ui.drawStatusBar("Map menu");
  c.fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, C_BG_RAISED);
  c.drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 8, C_ACCENT);
  c.setTextSize(1);
  for (int i = 0; i < N_ITEMS; i++) {
    int ry = MENU_Y + 8 + i * MENU_RH;
    bool on = (i == _sel);
    if (on) c.fillRoundRect(MENU_X + 4, ry, MENU_W - 8, MENU_RH - 4, 4, C_BG_ALT);
    c.setTextColor(on ? C_ACCENT : C_FG_FAINT);
    c.setCursor(MENU_X + 12, ry + 7);
    c.print(i + 1);
    c.setTextColor(on ? C_FG : C_FG_DIM);
    c.setCursor(MENU_X + 28, ry + 7);
    c.print(ITEMS[i]);
  }
  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, SCREEN_H - 12);
  c.print("up/down  enter=ok  back=close");
}

void MapOverlay::drawOver(UITask& ui, double clat, double clon) {
  GFXcanvas16& c = ui.cv();
  c.setTextSize(1);
  if (_mode == OFF) {
    c.fillRoundRect(CHIP_X, CHIP_Y, CHIP_W, CHIP_H, 4, C_BG_RAISED);
    c.drawRoundRect(CHIP_X, CHIP_Y, CHIP_W, CHIP_H, 4, C_ACCENT);
    c.setTextColor(C_FG);
    c.setCursor(CHIP_X + 9, CHIP_Y + 4);
    c.print("MENU");
    return;
  }
  if (_mode != PICK) return;

  // prominent crosshair: this is the point that gets saved
  const int cx = SCREEN_W / 2, cy = SCREEN_H / 2;
  c.drawCircle(cx, cy, 7, C_ACCENT);
  c.drawCircle(cx, cy, 8, C_BG);
  c.drawFastHLine(cx - 14, cy, 9, C_ACCENT);
  c.drawFastHLine(cx + 6, cy, 9, C_ACCENT);
  c.drawFastVLine(cx, cy - 14, 9, C_ACCENT);
  c.drawFastVLine(cx, cy + 6, 9, C_ACCENT);

  c.fillRoundRect(BAN_X, BAN_Y, BAN_W, BAN_H, 6, C_BG_RAISED);
  c.drawRoundRect(BAN_X, BAN_Y, BAN_W, BAN_H, 6, C_ACCENT);
  c.setTextColor(C_ACCENT);
  c.setCursor(BAN_X + 8, BAN_Y + 6);
  c.print("Set my position");
  char ll[40];
  fmtLatLon(ll, sizeof(ll), clat, clon);
  c.setTextColor(C_FG);
  c.setCursor(BAN_X + 8, BAN_Y + 20);
  c.print(ll);
  // what saving this means
  c.setCursor(BAN_X + 8, BAN_Y + 34);
  if (ui.gpsFix()) {
    c.setTextColor(C_FG_DIM);
    c.print("GPS fix wins; saved as backup");
  } else if (ui.prefs && ui.prefs->advert_loc_policy) {
    c.setTextColor(C_YELLOW);
    c.print("Shared in your adverts");
  } else {
    c.setTextColor(C_FG_DIM);
    c.print("Pan or tap the map");
  }
  c.setTextColor(C_FG_FAINT);
  c.setCursor(BAN_X + 8, BAN_Y + 48);
  c.print("click/enter=set  back=cancel");

  c.fillRoundRect(SET_X, BTN_Y, BTN_W, BTN_H, 4, C_ACCENT);
  c.setTextColor(C_BG);
  c.setCursor(SET_X + 18, BTN_Y + 8);
  c.print("Set");
  c.drawRoundRect(CAN_X, BTN_Y, BTN_W, BTN_H, 4, C_FG_DIM);
  c.setTextColor(C_FG_DIM);
  c.setCursor(CAN_X + 9, BTN_Y + 8);
  c.print("Cancel");
}

// ---------------------------------------------------------------- actions

void MapOverlay::run(UITask& ui, int item, double& clat, double& clon) {
  switch (item) {
    case 0:   // GPS info
      close();
      ui.go(SCR_GPS);
      break;
    case 1: { // Center on me
      double lat, lon;
      close();
      if (ui.ownPos(lat, lon)) { clat = lat; clon = lon; }
      else ui.toast("No position yet - see GPS info", C_YELLOW);
      break;
    }
    case 2:   // Set my position here
      openPick();
      break;
  }
}

void MapOverlay::confirmPick(UITask& ui, double clat, double clon) {
  ui.setManualPos(clat, clon);
  ui.toast(ui.gpsFix() ? "Saved as backup (GPS fix on)" : "Position set");
  close();
}

// ---------------------------------------------------------------- input

bool MapOverlay::key(UITask& ui, uint8_t k, double& clat, double& clon) {
  switch (_mode) {
    case OFF:
      if (k == 'm' || k == 'M' || k == 0x0D) { openMenu(); return true; }
      return false;
    case MENU:
      if (k == 0x0D) run(ui, _sel, clat, clon);
      else if (k >= '1' && k < '1' + N_ITEMS) run(ui, k - '1', clat, clon);
      else if (k == 0x1B || k == 0x08 || k == 0x7F || k == 'm' || k == 'M') close();
      return true;                      // menu swallows every key
    case PICK:
      if (k == 0x0D) { confirmPick(ui, clat, clon); return true; }
      if (k == 0x1B || k == 0x08 || k == 0x7F) { close(); ui.toast("Cancelled", C_FG_DIM); return true; }
      return false;                     // zoom keys etc. still go to the map
  }
  return false;
}

bool MapOverlay::nav(UITask& ui, NavEvent e, double& clat, double& clon) {
  switch (_mode) {
    case OFF:
      return false;
    case MENU:
      if (e == NAV_UP)   { if (_sel > 0) _sel--; }
      else if (e == NAV_DOWN) { if (_sel < N_ITEMS - 1) _sel++; }
      else if (e == NAV_SELECT || e == NAV_RIGHT) run(ui, _sel, clat, clon);
      else if (e == NAV_BACK || e == NAV_LEFT) close();
      return true;
    case PICK:
      if (e == NAV_SELECT) { confirmPick(ui, clat, clon); return true; }
      if (e == NAV_BACK)   { close(); ui.toast("Cancelled", C_FG_DIM); return true; }
      return false;                     // trackball pans the map under the crosshair
  }
  return false;
}

bool MapOverlay::touch(UITask& ui, const TouchEvent& e, double& clat, double& clon) {
  switch (_mode) {
    case OFF:
      if (e.kind == TouchEvent::TAP && inRect(e.x, e.y, CHIP_X, CHIP_Y, CHIP_W, CHIP_H, 6)) {
        openMenu();
        return true;
      }
      return false;
    case MENU:
      if (e.kind == TouchEvent::TAP) {
        if (inRect(e.x, e.y, MENU_X, MENU_Y + 8, MENU_W, N_ITEMS * MENU_RH)) {
          int i = (e.y - (MENU_Y + 8)) / MENU_RH;
          if (i >= 0 && i < N_ITEMS) { _sel = i; run(ui, i, clat, clon); }
        } else if (!inRect(e.x, e.y, MENU_X, MENU_Y, MENU_W, MENU_H)) {
          close();                      // tap outside the box
        }
      }
      return true;                      // swallow drags/lifts (no swipe-back)
    case PICK:
      if (e.kind == TouchEvent::LONG) return true;
      if (e.kind == TouchEvent::TAP) {
        if (inRect(e.x, e.y, SET_X, BTN_Y, BTN_W, BTN_H, 4)) { confirmPick(ui, clat, clon); return true; }
        if (inRect(e.x, e.y, CAN_X, BTN_Y, BTN_W, BTN_H, 4)) { close(); ui.toast("Cancelled", C_FG_DIM); return true; }
        if (inRect(e.x, e.y, BAN_X, BAN_Y, BAN_W, BAN_H)) return true;   // don't recentre under the banner
      }
      return false;                     // drag / tap-to-recentre still move the map
  }
  return false;
}
