#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

#define RP_ROW_H 24
#define RP_TOP (STATUS_H + 4)
#define RP_VIS ((SCREEN_H - RP_TOP - 16) / RP_ROW_H)

void RepeatersScreen::enter() {
  rebuild();
}

void RepeatersScreen::rebuild() {
  _n = 0;
  int n = ui.mesh->getNumContacts();
  for (int i = 0; i < n && _n < 24; i++) {
    ContactInfo ct;
    if (!ui.mesh->getContactByIdx(i, ct)) continue;
    if (ct.type != ADV_TYPE_REPEATER && ct.type != ADV_TYPE_ROOM) continue;
    memcpy(_prefixes[_n], ct.id.pub_key, 6);
    StrHelper::strncpy(_names[_n], ct.name, sizeof(_names[_n]));
    _types[_n] = ct.type;
    _last_adv[_n] = ct.lastmod;
    _n++;
  }
  if (_sel >= _n) _sel = _n ? _n - 1 : 0;
}

ContactInfo* RepeatersScreen::selContact() {
  if (_sel >= _n) return nullptr;
  return ui.mesh->lookupContactByPubKey(_prefixes[_sel], 6);
}

void RepeatersScreen::onCliResponse(const char* from, const char* text) {
  // shift up if full
  if (_cn >= 14) {
    memmove(&_clines[0], &_clines[1], sizeof(CLine) * 13);
    _cn = 13;
  }
  StrHelper::strncpy(_clines[_cn].from, from, sizeof(_clines[_cn].from));
  StrHelper::strncpy(_clines[_cn].text, text, sizeof(_clines[_cn].text));
  _cn++;
}

void RepeatersScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  c.setTextSize(1);

  if (!_console) {
    char title[40];
    snprintf(title, sizeof(title), "Repeaters (%d)", _n);
    ui.drawStatusBar(title);

    if (_n == 0) {
      c.setTextColor(C_FG_FAINT);
      c.setCursor(56, 110);
      c.print("No repeaters or rooms heard yet");
      return;
    }
    if (_sel < _top) _top = _sel;
    if (_sel >= _top + RP_VIS) _top = _sel - RP_VIS + 1;

    for (int i = _top; i < _n && i < _top + RP_VIS; i++) {
      int y = RP_TOP + (i - _top) * RP_ROW_H;
      bool sel = i == _sel;
      if (sel) c.fillRoundRect(2, y, SCREEN_W - 4, RP_ROW_H - 2, 5, C_BG_RAISED);
      c.fillRect(2, y, 3, RP_ROW_H - 2, _types[i] == ADV_TYPE_REPEATER ? C_ORANGE : C_PURPLE);
      c.setTextColor(sel ? C_FG : C_FG_DIM);
      c.setCursor(12, y + 3);
      c.print(_names[i]);
      char ago[8];
      ui.fmtAgo(ago, sizeof(ago), _last_adv[i]);
      char info[40];
      snprintf(info, sizeof(info), "%s   last advert %s",
               _types[i] == ADV_TYPE_REPEATER ? "repeater" : "room server", ago);
      c.setTextColor(C_FG_FAINT);
      c.setCursor(12, y + 13);
      c.print(info);
    }
    c.setTextColor(C_FG_FAINT);
    c.setCursor(6, SCREEN_H - 12);
    c.print("enter = console   l = login");
    return;
  }

  // ---- console mode ----
  char title[44];
  snprintf(title, sizeof(title), "%s console", _sel < _n ? _names[_sel] : "?");
  ui.drawStatusBar(title);

  int y = STATUS_H + 6;
  for (int i = 0; i < _cn; i++) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(4, y);
    c.print(_clines[i].from);
    c.setTextColor(C_TERM_RX);
    c.setCursor(4 + 11 * 6, y);
    c.print(_clines[i].text);
    y += 12;
  }

  // input line
  c.fillRect(0, SCREEN_H - 20, SCREEN_W, 20, C_BG_RAISED);
  c.setCursor(6, SCREEN_H - 14);
  if (_pwd_mode) {
    c.setTextColor(C_YELLOW);
    c.print("password: ");
    for (int i = 0; i < _llen; i++) c.write('*');
  } else if (_llen == 0) {
    c.setTextColor(C_FG_FAINT);
    c.print("command (or l = login, s = status, a = advert)");
  } else {
    c.setTextColor(C_FG);
    _line[_llen] = 0;
    c.print(_line);
  }
  c.fillRect(6 + (_llen + (_pwd_mode ? 10 : 0)) * 6 + 1, SCREEN_H - 15, 2, 10, C_ACCENT);
}

void RepeatersScreen::sendLine() {
  ContactInfo* ct = selContact();
  if (!ct || _llen == 0) { _llen = 0; return; }
  _line[_llen] = 0;
  uint32_t est_timeout;
  if (_pwd_mode) {
    int res = ui.mesh->sendLogin(*ct, _line, est_timeout);
    _pwd_mode = false;
    if (res == MSG_SEND_FAILED) ui.toast("Login send failed", C_RED);
    else { onCliResponse(">", "login sent, waiting..."); ui.termLog(C_TERM_TX, "[login->%s]", ct->name); }
  } else {
    int res = ui.mesh->sendCommandData(*ct, ui.epochNow(), 0, _line, est_timeout);
    if (res == MSG_SEND_FAILED) ui.toast("Send failed", C_RED);
    else { onCliResponse(">", _line); ui.termLog(C_TERM_TX, "[cmd->%s] %s", ct->name, _line); }
  }
  _llen = 0;
}

bool RepeatersScreen::key(uint8_t k) {
  if (!_console) {
    if (k == 0x0D) { if (_n) { _console = true; _cn = 0; } return true; }
    if (k == 'l' && _n) { _console = true; _cn = 0; _pwd_mode = true; return true; }
    return false;
  }
  // console mode
  if (k == 0x0D) { sendLine(); return true; }
  if (k == 0x08) {
    if (_llen > 0) { _llen--; return true; }
    _console = false;   // back to list
    return true;
  }
  if (_llen == 0 && !_pwd_mode) {
    // quick keys when line empty
    if (k == 'l') { _pwd_mode = true; return true; }
    if (k == 's') { strcpy(_line, "stats"); _llen = 5; sendLine(); return true; }
    if (k == 'a') { strcpy(_line, "advert"); _llen = 6; sendLine(); return true; }
    if (k == 'c') {   // sync repeater clock to our timestamp
      strcpy(_line, "clock sync");
      _llen = 10;
      sendLine();
      return true;
    }
    if (k == 'n') { strcpy(_line, "neighbors"); _llen = 9; sendLine(); return true; }
  }
  if (k >= 32 && k < 127 && _llen < (int)sizeof(_line) - 2) {
    _line[_llen++] = k;
    return true;
  }
  return false;
}

bool RepeatersScreen::nav(NavEvent e) {
  if (_console) {
    if (e == NAV_SELECT) { sendLine(); return true; }
    if (e == NAV_BACK) { _console = false; _pwd_mode = false; _llen = 0; return true; }
    return false;
  }
  switch (e) {
    case NAV_UP:   if (_sel > 0) _sel--; return true;
    case NAV_DOWN: if (_sel < _n - 1) _sel++; return true;
    case NAV_SELECT: if (_n) { _console = true; _cn = 0; } return true;
    default: return false;
  }
}
