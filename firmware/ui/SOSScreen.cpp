#include "AllScreens.h"
#include "../MyMesh.h"

/*
 * SOS beacon screen.
 *
 *   Idle    a short form: your position, Send to (any channel; public ones
 *           are marked), the exact message, and a Start SOS button. Only the
 *           Start button starts a beacon; a stray tap elsewhere does nothing.
 *   Active  the whole screen is red; any press stops the beacon.
 *
 * The beacon itself is unchanged: UITask repeats the message every 2 minutes
 * on the chosen channel until it is stopped.
 */

#define SO_ROW_X    8
#define SO_ROW_W    (SCREEN_W - 16)
#define SO_ROW_H    26
#define SO_POS_Y    (STATUS_H + 50)
#define SO_CHAN_Y   (SO_POS_Y + 20)
#define SO_BTN_H    32
#define SO_BTN_Y    (SCREEN_H - 14 - SO_BTN_H - 4)
#define SO_LIST_TOP (STATUS_H + 24)
#define SO_LIST_RH  22
#define SO_FOOT     14

enum { F_CHAN = 0, F_START, F_N };

static bool isBackKey(uint8_t k) { return k == 0x1B || k == 0x08 || k == 0x7F || k == '`'; }

static bool inRow(int x, int y, int ry, int rh) {
  return x >= SO_ROW_X && x < SO_ROW_X + SO_ROW_W && y >= ry && y < ry + rh;
}

static int listRows() { return (SCREEN_H - SO_LIST_TOP - SO_FOOT) / SO_LIST_RH; }

// ---------------------------------------------------------------- state

void SOSScreen::enter() {
  _picking = false;
  char nm[32];
  if (!ui.channelNameAt(_chan, nm, sizeof(nm))) _chan = ui.channelSlotAt(0);   // channel removed
  if (_chan < 0) _chan = 0;
}

void SOSScreen::activate(int field) {
  if (field == F_CHAN) {
    _picking = true;
    _lsel = 0;
    for (int i = 0; i < ui.channelCount(); i++) if (ui.channelSlotAt(i) == _chan) _lsel = i;
    _top = 0;
    _drag_acc = 0;
  } else {
    ui.startSOSHere(_chan);
  }
}

// ---------------------------------------------------------------- drawing

void SOSScreen::draw() {
  if (ui.sosActive()) drawActive();
  else if (_picking) drawChannels();
  else drawForm();
}

void SOSScreen::drawActive() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_RED);
  ui.drawStatusBar("SOS Beacon");
  c.setTextSize(3);
  c.setTextColor(0xFFFF);
  c.setCursor(120, 50);
  c.print("SOS");
  c.setTextSize(1);
  c.setCursor(40, 96);
  c.print("ACTIVE - broadcasting every 2 min");

  double lat, lon;
  char line[64], ch[32] = "";
  if (ui.ownPos(lat, lon)) snprintf(line, sizeof(line), "pos: %.5f, %.5f", lat, lon);
  else snprintf(line, sizeof(line), "pos: lost");
  c.setCursor(40, 124);
  c.print(line);
  ui.channelNameAt(ui.sosChannel(), ch, sizeof(ch));
  snprintf(line, sizeof(line), "to: %s", ch);
  c.setCursor(40, 138);
  c.print(line);

  c.setTextSize(2);
  c.setCursor(76, 186);
  c.print("Press to STOP");
  c.setTextSize(1);
}

void SOSScreen::drawForm() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("SOS Beacon");

  c.setTextSize(3);
  c.setTextColor(C_RED);
  c.setCursor(12, STATUS_H + 8);
  c.print("SOS");
  c.setTextSize(1);
  c.setTextColor(C_FG);
  c.setCursor(80, STATUS_H + 10);
  c.print("Emergency location beacon");
  c.setTextColor(C_FG_FAINT);
  c.setCursor(80, STATUS_H + 22);
  c.print("repeats every 2 min until stopped");

  // position (from GPS or the manual position in Settings)
  double lat = 0, lon = 0;
  const bool have = ui.ownPos(lat, lon);
  char v[64];
  if (have) snprintf(v, sizeof(v), "%.5f, %.5f", lat, lon);
  else snprintf(v, sizeof(v), "not set - GPS or manual in Settings");
  c.setTextColor(C_FG_FAINT);
  c.setCursor(SO_ROW_X + 8, SO_POS_Y);
  c.print("Position");
  c.setTextColor(have ? C_FG : C_YELLOW);
  c.setCursor(SO_ROW_X + 64, SO_POS_Y);
  c.print(v);

  // Send to
  const bool on_c = _sel == F_CHAN;
  c.fillRoundRect(SO_ROW_X, SO_CHAN_Y, SO_ROW_W, SO_ROW_H - 2, 4, on_c ? C_BG_RAISED : C_BG_ALT);
  if (on_c) c.drawRoundRect(SO_ROW_X, SO_CHAN_Y, SO_ROW_W, SO_ROW_H - 2, 4, C_ACCENT);
  c.setTextColor(C_FG_FAINT);
  c.setCursor(SO_ROW_X + 8, SO_CHAN_Y + 8);
  c.print("Send to");
  char ch[32] = "", chs[22];
  ui.channelNameAt(_chan, ch, sizeof(ch));
  ellipsize(chs, sizeof(chs), ch);
  c.setTextColor(C_FG);
  c.setCursor(SO_ROW_X + 64, SO_CHAN_Y + 8);
  c.print(chs);
  if (ui.channelIsPublic(_chan)) {          // everyone on the mesh will see it
    c.setTextColor(C_YELLOW);
    c.setCursor(SO_ROW_X + SO_ROW_W - 12 - 7 * 6, SO_CHAN_Y + 8);
    c.print("public");
  }
  c.setTextColor(C_FG_DIM);
  c.setCursor(SO_ROW_X + SO_ROW_W - 12, SO_CHAN_Y + 8);
  c.print(">");

  // exactly what gets sent
  if (have) {
    char msg[96], shown[(SCREEN_W - 16) / 6 + 1];
    ui.fmtOwnSOS(msg, sizeof(msg), lat, lon);
    ellipsize(shown, sizeof(shown), msg);
    c.setTextColor(C_FG_DIM);
    c.setCursor(SO_ROW_X, SO_CHAN_Y + SO_ROW_H + 10);
    c.print(shown);
  }

  // Start
  const bool on_s = _sel == F_START;
  c.fillRoundRect(SO_ROW_X, SO_BTN_Y, SO_ROW_W, SO_BTN_H, 6, have ? C_RED : C_BG_ALT);
  if (on_s) {
    c.drawRoundRect(SO_ROW_X, SO_BTN_Y, SO_ROW_W, SO_BTN_H, 6, 0xFFFF);
    c.drawRoundRect(SO_ROW_X + 1, SO_BTN_Y + 1, SO_ROW_W - 2, SO_BTN_H - 2, 5, 0xFFFF);
  }
  c.setTextSize(2);
  c.setTextColor(have ? 0xFFFF : C_FG_FAINT);
  c.setCursor((SCREEN_W - 9 * 12) / 2, SO_BTN_Y + 9);
  c.print("Start SOS");
  c.setTextSize(1);

  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, SCREEN_H - 11);
  c.print("up/down  enter=choose  back=close");
}

void SOSScreen::drawChannels() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("SOS: send to?");
  c.setTextSize(1);
  const int n = ui.channelCount(), rows = listRows();
  if (_lsel < _top) _top = _lsel;
  if (_lsel >= _top + rows) _top = _lsel - rows + 1;
  for (int r = 0; r < rows && _top + r < n; r++) {
    const int li = _top + r;
    const int ry = SO_LIST_TOP + r * SO_LIST_RH;
    const bool on = li == _lsel;
    c.fillRoundRect(SO_ROW_X, ry, SO_ROW_W, SO_LIST_RH - 3, 4, on ? C_BG_RAISED : C_BG_ALT);
    if (on) c.drawRoundRect(SO_ROW_X, ry, SO_ROW_W, SO_LIST_RH - 3, 4, C_ACCENT);
    const int slot = ui.channelSlotAt(li);
    char nm[32] = "", shown[30];
    ui.channelNameAt(slot, nm, sizeof(nm));
    ellipsize(shown, sizeof(shown), nm);
    c.setTextColor(on ? C_FG : C_FG_DIM);
    c.setCursor(SO_ROW_X + 10, ry + 6);
    c.print(shown);
    if (ui.channelIsPublic(slot)) {
      c.setTextColor(C_YELLOW);
      c.setCursor(SO_ROW_X + SO_ROW_W - 8 - 6 * 6, ry + 6);
      c.print("public");
    }
  }
  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, SCREEN_H - 11);
  c.print("up/down  enter=choose  back=cancel");
}

// ---------------------------------------------------------------- input

bool SOSScreen::key(uint8_t k) {
  if (ui.sosActive()) {
    if (k == 0x0D) { ui.toggleSOS(); return true; }
    return false;                           // back keys leave; the beacon keeps going
  }
  if (!_picking) {
    if (k == 0x0D) { activate(_sel); return true; }
    return false;                           // i/k nav fallback, back keys leave
  }
  if (k == 0x0D) { _chan = ui.channelSlotAt(_lsel); _picking = false; _sel = F_START; return true; }
  if (isBackKey(k)) { _picking = false; return true; }
  return false;
}

bool SOSScreen::nav(NavEvent e) {
  if (ui.sosActive()) {
    if (e == NAV_SELECT) { ui.toggleSOS(); return true; }
    return false;
  }
  if (!_picking) {
    if (e == NAV_UP)   { if (_sel > 0) _sel--; return true; }
    if (e == NAV_DOWN) { if (_sel < F_N - 1) _sel++; return true; }
    if (e == NAV_SELECT || e == NAV_RIGHT) { activate(_sel); return true; }
    return false;                           // BACK leaves the screen
  }
  if (e == NAV_UP)   { if (_lsel > 0) _lsel--; return true; }
  if (e == NAV_DOWN) { if (_lsel < ui.channelCount() - 1) _lsel++; return true; }
  if (e == NAV_SELECT || e == NAV_RIGHT) { _chan = ui.channelSlotAt(_lsel); _picking = false; _sel = F_START; return true; }
  if (e == NAV_BACK || e == NAV_LEFT) { _picking = false; return true; }
  return true;
}

bool SOSScreen::touch(const TouchEvent& e) {
  if (ui.sosActive()) {
    if (e.kind == TouchEvent::TAP) { ui.toggleSOS(); return true; }
    return false;
  }
  if (!_picking) {
    if (e.kind != TouchEvent::TAP) return false;   // swipe right = back still works
    if (inRow(e.x, e.y, SO_CHAN_Y, SO_ROW_H)) { _sel = F_CHAN; activate(F_CHAN); return true; }
    if (inRow(e.x, e.y, SO_BTN_Y, SO_BTN_H))  { _sel = F_START; activate(F_START); return true; }
    return true;                            // a stray tap elsewhere does nothing
  }
  const int n = ui.channelCount();
  if (e.kind == TouchEvent::DRAG) {
    _drag_acc += e.dy;
    while (_drag_acc <= -SO_LIST_RH && _top < n - listRows()) { _top++; _drag_acc += SO_LIST_RH; }
    while (_drag_acc >= SO_LIST_RH && _top > 0) { _top--; _drag_acc -= SO_LIST_RH; }
    if (_lsel < _top) _lsel = _top;
    if (_lsel >= _top + listRows()) _lsel = _top + listRows() - 1;
    return true;
  }
  if (e.kind == TouchEvent::RELEASE) {
    _drag_acc = 0;
    if (e.dx > 90 && abs(e.dy) < 60) _picking = false;   // swipe right = cancel
    return true;
  }
  if (e.kind == TouchEvent::TAP && e.y >= SO_LIST_TOP) {
    int li = _top + (e.y - SO_LIST_TOP) / SO_LIST_RH;
    if (li >= 0 && li < n) { _chan = ui.channelSlotAt(li); _picking = false; _sel = F_START; }
  }
  return true;
}
