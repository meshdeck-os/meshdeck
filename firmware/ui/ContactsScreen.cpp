#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/AdvertDataHelpers.h>

#define ROW_H 24
#define LIST_TOP (STATUS_H + 2)
#define VISIBLE ((SCREEN_H - LIST_TOP) / ROW_H)

static const char* ACTIONS[] = { "Message", "Trace route", "Reset path", "Share (0-hop)", "Remove" };
#define N_ACTIONS 5

void ContactsScreen::enter() {
  _menu = -1;
  int n = ui.mesh->getNumContacts();
  if (_sel >= n) _sel = n ? n - 1 : 0;
}

void ContactsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  int n = ui.mesh->getNumContacts();
  char title[40];
  snprintf(title, sizeof(title), "Contacts (%d)", n);
  ui.drawStatusBar(title);

  if (n == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(52, 110);
    c.print("No contacts yet - waiting for adverts");
    return;
  }

  if (_sel < _top) _top = _sel;
  if (_sel >= _top + VISIBLE) _top = _sel - VISIBLE + 1;

  c.setTextSize(1);
  for (int i = _top; i < n && i < _top + VISIBLE; i++) {
    ContactInfo ct;
    if (!ui.mesh->getContactByIdx(i, ct)) continue;
    int y = LIST_TOP + (i - _top) * ROW_H;
    bool sel = i == _sel;
    if (sel) c.fillRoundRect(2, y, SCREEN_W - 4, ROW_H - 2, 5, C_BG_RAISED);

    // type badge
    uint16_t tc = ct.type == ADV_TYPE_REPEATER ? C_ORANGE :
                  ct.type == ADV_TYPE_ROOM ? C_PURPLE :
                  ct.type == ADV_TYPE_SENSOR ? C_YELLOW : C_CYAN;
    c.fillCircle(12, y + ROW_H / 2 - 1, 5, tc);
    c.setTextColor(C_BG);
    c.setCursor(10, y + ROW_H / 2 - 4);
    c.print(ct.type == ADV_TYPE_REPEATER ? "R" : ct.type == ADV_TYPE_ROOM ? "O" : "C");

    c.setTextColor(sel ? C_FG : C_FG_DIM);
    char nm[26];
    ellipsize(nm, sizeof(nm), ct.name);
    c.setCursor(24, y + 3);
    c.print(nm);

    // path + last advert info
    char info[40];
    char ago[8];
    ui.fmtAgo(ago, sizeof(ago), ct.lastmod);
    if (ct.out_path_len == 0xFF) snprintf(info, sizeof(info), "flood  %s", ago);
    else snprintf(info, sizeof(info), "%d hop%s  %s", ct.out_path_len, ct.out_path_len == 1 ? "" : "s", ago);
    c.setTextColor(C_FG_FAINT);
    c.setCursor(24, y + 13);
    c.print(info);
    // location marker
    if (ct.gps_lat || ct.gps_lon) {
      c.setTextColor(C_GREEN);
      c.setCursor(SCREEN_W - 20, y + 8);
      c.print("*");
    }
  }

  // scrollbar
  if (n > VISIBLE) {
    int bar_h = (SCREEN_H - LIST_TOP) * VISIBLE / n;
    int bar_y = LIST_TOP + (SCREEN_H - LIST_TOP - bar_h) * _top / (n - VISIBLE);
    c.fillRect(SCREEN_W - 3, bar_y, 2, bar_h, C_FG_FAINT);
  }

  // action menu popup
  if (_menu >= 0) {
    int mw = 130, mh = N_ACTIONS * 18 + 10;
    int mx = SCREEN_W - mw - 14, my = 60;
    c.fillRoundRect(mx, my, mw, mh, 8, C_BG_RAISED);
    c.drawRoundRect(mx, my, mw, mh, 8, C_ACCENT);
    for (int i = 0; i < N_ACTIONS; i++) {
      c.setTextColor(i == _menu ? C_ACCENT : C_FG_DIM);
      c.setCursor(mx + 12, my + 8 + i * 18);
      c.print(ACTIONS[i]);
    }
  }
}

void ContactsScreen::action(int which) {
  ContactInfo ct;
  if (!ui.mesh->getContactByIdx(_sel, ct)) return;
  _menu = -1;
  switch (which) {
    case 0: {   // message
      DeckThread* t = ui.store.forContact(ct.id.pub_key, ct.name);
      if (t) ui.openThread(ui.store.indexOf(t));
      break;
    }
    case 1: {   // trace
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (live && ui.startTrace(*live)) ui.go(SCR_TRACE);
      break;
    }
    case 2: {   // reset path
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (live) {
        ui.mesh->resetPathTo(*live);
        ui.toast("Path reset - next send floods");
      }
      break;
    }
    case 3: {   // share zero hop
      ui.mesh->shareContactZeroHop(ct);
      ui.toast("Contact shared 0-hop");
      break;
    }
    case 4: {   // remove
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (live && ui.mesh->removeContact(*live)) {
        ui.toast("Contact removed", C_YELLOW);
        if (_sel > 0) _sel--;
      }
      break;
    }
  }
}

bool ContactsScreen::key(uint8_t k) {
  if (k == 0x0D) {
    if (_menu >= 0) action(_menu);
    else _menu = 0;
    return true;
  }
  if (k == 'm') { action(0); return true; }
  if (k == 't') { action(1); return true; }
  return false;
}

bool ContactsScreen::nav(NavEvent e) {
  int n = ui.mesh->getNumContacts();
  if (_menu >= 0) {
    switch (e) {
      case NAV_UP:   if (_menu > 0) _menu--; return true;
      case NAV_DOWN: if (_menu < N_ACTIONS - 1) _menu++; return true;
      case NAV_SELECT: action(_menu); return true;
      case NAV_BACK: _menu = -1; return true;
      default: return true;
    }
  }
  switch (e) {
    case NAV_UP:   if (_sel > 0) _sel--; return true;
    case NAV_DOWN: if (_sel < n - 1) _sel++; return true;
    case NAV_SELECT: if (n) _menu = 0; return true;
    default: return false;
  }
}

bool ContactsScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  if (_menu >= 0) {
    _menu = -1;
    return true;
  }
  int idx = _top + (e.y - LIST_TOP) / ROW_H;
  if (idx >= 0 && idx < ui.mesh->getNumContacts()) {
    if (idx == _sel) _menu = 0;
    else _sel = idx;
  }
  return true;
}
