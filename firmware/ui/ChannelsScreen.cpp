#include "AllScreens.h"
#include <helpers/TxtDataHelpers.h>

/*
 * Channels: list the group channels you're on (Public is there by default),
 * tap one to open its chat, or add a new one by name + shared key. Text entry
 * uses the same number/symbol layer (roll trackball up, or Alt+C).
 */

#define CH_ROW_H 20
#define CH_TOP   (STATUS_H + 6)
#define CH_VIS   ((SCREEN_H - CH_TOP - 14) / CH_ROW_H)

void ChannelsScreen::enter() {
  _adding = false;
  _phase = 0;
  _sel = 0;
  _top = 0;
  _n = ui.channelCount();
}

void ChannelsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Channels");
  c.setTextSize(1);
  _n = ui.channelCount();

  if (_adding) {
    c.setTextColor(C_ACCENT);
    c.setCursor(12, CH_TOP + 6);
    c.print(_phase == 0 ? "New channel name:" : "Channel key (base64):");
    _edit[_elen] = 0;
    c.fillRect(12, CH_TOP + 26, SCREEN_W - 24, 18, C_BG);
    c.drawRect(12, CH_TOP + 26, SCREEN_W - 24, 18, C_ACCENT);
    c.setTextColor(C_YELLOW);
    c.setCursor(18, CH_TOP + 31);
    c.print(_edit);
    c.fillRect(18 + _elen * 6 + 1, CH_TOP + 30, 2, 11, C_ACCENT);
    if (ui.symShift()) { c.setTextColor(C_CYAN); c.setCursor(SCREEN_W - 34, CH_TOP + 31); c.print("123"); }
    if (_phase == 1) {
      c.setTextColor(C_FG_FAINT);
      c.setCursor(12, CH_TOP + 52);
      c.print("padding added automatically");
    }
    c.setTextColor(ui.symShift() ? C_CYAN : C_FG_FAINT);
    c.setCursor(6, SCREEN_H - 10);
    c.print(ui.symShift() ? "123 mode  enter=next" : "roll ball up=123  enter=next");
    return;
  }

  int total = _n + 1;   // channels + "add" row
  if (_sel < _top) _top = _sel;
  if (_sel >= _top + CH_VIS) _top = _sel - CH_VIS + 1;

  for (int r = _top; r < total && r < _top + CH_VIS; r++) {
    int y = CH_TOP + (r - _top) * CH_ROW_H;
    bool sel = r == _sel;
    if (sel) c.fillRoundRect(4, y - 1, SCREEN_W - 8, CH_ROW_H - 3, 5, C_BG_RAISED);
    if (r < _n) {
      char nm[32];
      if (!ui.channelNameAt(r, nm, sizeof(nm))) strcpy(nm, "?");
      c.setTextColor(sel ? C_FG : C_FG_DIM);
      c.setCursor(12, y + 4);
      c.print("# ");
      c.print(nm);
      if (sel) { c.setTextColor(C_ACCENT); c.setCursor(SCREEN_W - 8 - 4 * 6, y + 4); c.print("open"); }
    } else {
      c.setTextColor(sel ? C_GREEN : C_FG_DIM);
      c.setCursor(12, y + 4);
      c.print("+ Add channel");
    }
  }

  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 10);
  c.print("up/down = choose   enter = open / add");
}

void ChannelsScreen::select() {
  if (_sel < _n) {
    ui.openChannel(_sel);
  } else {
    _adding = true;
    _phase = 0;
    _edit[0] = 0;
    _elen = 0;
    _newname[0] = 0;
  }
}

void ChannelsScreen::applyEdit() {
  _edit[_elen] = 0;
  if (_phase == 0) {
    StrHelper::strncpy(_newname, _edit, sizeof(_newname));
    _phase = 1;
    _edit[0] = 0;
    _elen = 0;
  } else {
    ui.addChannelNamed(_newname, _edit);
    _adding = false;
    _phase = 0;
    _n = ui.channelCount();
    _sel = 0;
  }
}

bool ChannelsScreen::key(uint8_t k) {
  if (_adding) {
    if (k == 0x0D) { applyEdit(); return true; }
    if (k == 0x08 || k == 0x7F) {
      if (_elen > 0) _elen--;
      else _adding = false;
      return true;
    }
    if (k >= 32 && k < 127 && _elen < (int)sizeof(_edit) - 2) { _edit[_elen++] = k; return true; }
    return true;
  }
  if (k == 0x0D) { select(); return true; }
  return false;
}

bool ChannelsScreen::nav(NavEvent e) {
  if (_adding) {
    if (e == NAV_SELECT) { applyEdit(); return true; }
    if (e == NAV_BACK) { _adding = false; return true; }
    if (e == NAV_UP   && !ui.symShift()) ui.toggleSym();
    if (e == NAV_DOWN &&  ui.symShift()) ui.toggleSym();
    return true;
  }
  switch (e) {
    case NAV_UP:     if (_sel > 0) _sel--; return true;
    case NAV_DOWN:   if (_sel < _n) _sel++; return true;   // _n = the "add" row
    case NAV_SELECT: select(); return true;
    default: return false;
  }
}

bool ChannelsScreen::touch(const TouchEvent& e) {
  if (_adding) return false;
  if (e.kind != TouchEvent::TAP) return false;
  if (e.y < CH_TOP) return false;
  int r = _top + (e.y - CH_TOP) / CH_ROW_H;
  if (r >= 0 && r <= _n) { _sel = r; select(); return true; }
  return false;
}
