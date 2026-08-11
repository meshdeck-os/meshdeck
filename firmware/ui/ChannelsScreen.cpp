#include "AllScreens.h"
#include <helpers/TxtDataHelpers.h>
#include <string.h>

/*
 * Channels: list group channels (Public is seeded at boot), open chat, or
 * manage them like Contacts - rename, rekey, show/share key, remove.
 *
 * Add flow:
 *   1) name  2) key (base64)
 *   - blank key + name starting with '#' -> hashtag channel (sha256-derived key)
 *   - blank key otherwise -> private channel with a fresh random key
 *   - pasted base64 -> join/create with that shared secret
 *
 * Empty slots (blank name + zero secret) are hidden; delete clears a slot
 * the same way the companion protocol does.
 */

#define CH_ROW_H 22
#define CH_TOP   (STATUS_H + 6)
#define CH_VIS   ((SCREEN_H - CH_TOP - 14) / CH_ROW_H)

static const char* CH_ACTIONS[] = {
  "Open chat",
  "Rename",
  "Change key",
  "Show key",
  "Share key (QR)",
  "Remove",
};
#define N_CH_ACTIONS ((int)(sizeof(CH_ACTIONS) / sizeof(CH_ACTIONS[0])))

void ChannelsScreen::refreshCount() {
  _n = ui.channelCount();
  if (_sel > _n) _sel = _n;   // allow sel == _n for "+ Add"
  if (_sel < 0) _sel = 0;
  if (_top > _sel) _top = _sel;
  if (_top < 0) _top = 0;
}

int ChannelsScreen::meshIndex() const {
  if (_sel < 0 || _sel >= _n) return -1;
  return ui.channelSlotAt(_sel);
}

void ChannelsScreen::enter() {
  _mode = LIST;
  _menu = 0;
  _edit_slot = -1;
  _elen = 0;
  _edit[0] = 0;
  _newname[0] = 0;
  _show_key[0] = 0;
  _show_name[0] = 0;
  _sel = 0;
  _top = 0;
  refreshCount();
}

void ChannelsScreen::drawEditor(const char* title, const char* hint) {
  GFXcanvas16& c = ui.cv();
  c.setTextColor(C_ACCENT);
  c.setCursor(12, CH_TOP + 6);
  c.print(title);

  _edit[_elen] = 0;
  c.fillRect(12, CH_TOP + 26, SCREEN_W - 24, 18, C_BG);
  c.drawRect(12, CH_TOP + 26, SCREEN_W - 24, 18, C_ACCENT);
  c.setTextColor(C_YELLOW);
  c.setCursor(18, CH_TOP + 31);
  // scroll long keys so the end stays visible
  const char* shown = _edit;
  int maxch = (SCREEN_W - 40) / 6;
  if (_elen > maxch) shown = _edit + (_elen - maxch);
  c.print(shown);
  int vislen = (int)strlen(shown);
  c.fillRect(18 + vislen * 6 + 1, CH_TOP + 30, 2, 11, C_ACCENT);

  if (ui.symShift()) {
    c.setTextColor(C_CYAN);
    c.setCursor(SCREEN_W - 34, CH_TOP + 31);
    c.print("123");
  }
  if (hint && hint[0]) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(12, CH_TOP + 52);
    c.print(hint);
  }
  c.setTextColor(ui.symShift() ? C_CYAN : C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 10);
  c.print(ui.symShift() ? "123 mode  enter=ok  bksp=back" : "roll up=123  enter=ok  bksp=back");
}

void ChannelsScreen::drawShowKey() {
  GFXcanvas16& c = ui.cv();
  c.setTextColor(C_ACCENT);
  c.setCursor(12, CH_TOP + 8);
  c.print("Channel key");

  c.setTextColor(C_CYAN);
  c.setCursor(12, CH_TOP + 28);
  {
    char line[40];
    ellipsize(line, sizeof(line), _show_name);
    c.print("# ");
    c.print(line);
  }

  c.setTextColor(C_FG_FAINT);
  c.setCursor(12, CH_TOP + 48);
  c.print("base64 secret (share carefully):");

  // wrap key across a few lines
  c.setTextColor(C_YELLOW);
  const char* k = _show_key;
  int y = CH_TOP + 66;
  int col = 0;
  const int cols = 36;
  c.setCursor(12, y);
  while (*k && y < SCREEN_H - 28) {
    c.print(*k++);
    if (++col >= cols) {
      col = 0;
      y += 12;
      c.setCursor(12, y);
    }
  }

  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 10);
  c.print("enter/back = close   QR via menu Share");
}

void ChannelsScreen::drawConfirmDel() {
  GFXcanvas16& c = ui.cv();
  // dim
  for (int y = 0; y < SCREEN_H; y += 2)
    c.drawFastHLine(0, y, SCREEN_W, C_BG);

  const int bw = 280, bh = 100;
  const int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;
  c.fillRoundRect(bx, by, bw, bh, 8, C_BG_RAISED);
  c.drawRoundRect(bx, by, bw, bh, 8, C_RED);

  c.setTextSize(1);
  c.setTextColor(C_FG);
  c.setCursor(bx + 14, by + 16);
  c.print("Remove channel?");

  c.setTextColor(C_CYAN);
  c.setCursor(bx + 14, by + 36);
  {
    char line[40];
    ellipsize(line, sizeof(line), _show_name);
    c.print("# ");
    c.print(line);
  }

  c.setTextColor(C_FG_FAINT);
  c.setCursor(bx + 14, by + 52);
  c.print("Chat history is kept as ~name");

  c.fillRoundRect(bx + 20, by + 70, 100, 22, 5, C_RED);
  c.fillRoundRect(bx + 150, by + 70, 100, 22, 5, C_BG_ALT);
  c.drawRoundRect(bx + 150, by + 70, 100, 22, 5, C_FG_DIM);
  c.setTextColor(C_BG);
  c.setCursor(bx + 40, by + 77);
  c.print("Remove");
  c.setTextColor(C_FG);
  c.setCursor(bx + 172, by + 77);
  c.print("Cancel");
}

void ChannelsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  refreshCount();

  char title[40];
  snprintf(title, sizeof(title), "Channels (%d)", _n);
  ui.drawStatusBar(title);
  c.setTextSize(1);

  if (_mode == ADD_NAME) {
    drawEditor("Add channel - name:",
               "e.g. team  or  #topic  (hashtag)");
    return;
  }
  if (_mode == ADD_KEY) {
    drawEditor("Channel key (base64):",
               "blank = #tag hash or random key");
    return;
  }
  if (_mode == RENAME) {
    drawEditor("Rename channel:", "type a new display name");
    return;
  }
  if (_mode == REKEY) {
    drawEditor("New key (base64):", "blank = generate random key");
    return;
  }
  if (_mode == SHOW_KEY) {
    drawShowKey();
    return;
  }
  if (_mode == CONFIRM_DEL) {
    // list is dimmed underneath via dialog
    // (draw dialog only - full screen dim)
    drawConfirmDel();
    return;
  }

  // ---- list ----
  int total = _n + 1;   // channels + "add" row
  if (_sel < _top) _top = _sel;
  if (_sel >= _top + CH_VIS) _top = _sel - CH_VIS + 1;

  if (_n == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(40, 100);
    c.print("No channels yet");
    c.setCursor(24, 116);
    c.print("Add Public is seeded at boot");
  }

  for (int r = _top; r < total && r < _top + CH_VIS; r++) {
    int y = CH_TOP + (r - _top) * CH_ROW_H;
    bool sel = r == _sel && _mode == LIST;
    if (sel) c.fillRoundRect(4, y - 1, SCREEN_W - 8, CH_ROW_H - 3, 5, C_BG_RAISED);

    if (r < _n) {
      int mi = ui.channelSlotAt(r);
      char nm[32];
      if (mi < 0 || !ui.channelNameAt(mi, nm, sizeof(nm))) strcpy(nm, "?");

      bool is_pub = mi >= 0 && ui.channelIsPublic(mi);
      c.setTextColor(is_pub ? C_ORANGE : (sel ? C_FG : C_FG_DIM));
      c.setCursor(12, y + 5);
      c.print("# ");
      {
        char line[28];
        ellipsize(line, sizeof(line), nm);
        c.print(line);
      }
      if (is_pub) {
        c.setTextColor(C_FG_FAINT);
        c.setCursor(SCREEN_W - 8 - 6 * 6, y + 5);
        c.print("public");
      } else if (sel) {
        c.setTextColor(C_ACCENT);
        c.setCursor(SCREEN_W - 8 - 4 * 6, y + 5);
        c.print("menu");
      }
    } else {
      c.setTextColor(sel ? C_GREEN : C_FG_DIM);
      c.setCursor(12, y + 5);
      c.print("+ Add channel");
    }
  }

  if (_mode == MENU && _sel < _n) {
    int mw = 150, mh = N_CH_ACTIONS * 16 + 12;
    int mx = SCREEN_W - mw - 10, my = 50;
    // keep menu on-screen near selection
    int row_y = CH_TOP + (_sel - _top) * CH_ROW_H;
    if (row_y + mh < SCREEN_H - 8) my = row_y;
    if (my + mh > SCREEN_H - 6) my = SCREEN_H - 6 - mh;
    if (my < STATUS_H + 4) my = STATUS_H + 4;

    c.fillRoundRect(mx, my, mw, mh, 8, C_BG_RAISED);
    c.drawRoundRect(mx, my, mw, mh, 8, C_ACCENT);
    for (int i = 0; i < N_CH_ACTIONS; ++i) {
      bool on = i == _menu;
      if (on) c.fillRoundRect(mx + 3, my + 5 + i * 16, mw - 6, 15, 3, C_BG_ALT);
      c.setTextColor(on ? C_ACCENT : C_FG_DIM);
      c.setCursor(mx + 12, my + 8 + i * 16);
      c.print(CH_ACTIONS[i]);
    }
  }

  if (_mode == LIST) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(6, SCREEN_H - 10);
    c.print("up/down  enter=open/menu  +add");
  }
}

void ChannelsScreen::beginAdd() {
  _mode = ADD_NAME;
  _edit_slot = -1;
  _newname[0] = 0;
  _edit[0] = 0;
  _elen = 0;
}

void ChannelsScreen::openMenu() {
  int mi = meshIndex();
  if (mi < 0) return;
  _edit_slot = mi;
  _mode = MENU;
  _menu = 0;
}

void ChannelsScreen::runAction(int which) {
  int mi = _edit_slot >= 0 ? _edit_slot : meshIndex();
  if (mi < 0) { _mode = LIST; return; }
  _mode = LIST;

  switch (which) {
    case 0:  // Open chat
      ui.openChannel(mi);
      break;
    case 1: {  // Rename
      char nm[32];
      if (!ui.channelNameAt(mi, nm, sizeof(nm))) break;
      _edit_slot = mi;
      StrHelper::strncpy(_edit, nm, sizeof(_edit));
      _elen = (int)strlen(_edit);
      _mode = RENAME;
      break;
    }
    case 2: {  // Change key
      if (ui.channelIsPublic(mi)) {
        ui.toast("Can't rekey Public", C_YELLOW);
        break;
      }
      _edit_slot = mi;
      _edit[0] = 0;
      _elen = 0;
      _mode = REKEY;
      break;
    }
    case 3: {  // Show key
      if (!ui.channelKeyBase64(mi, _show_key, sizeof(_show_key))) {
        ui.toast("No key", C_YELLOW);
        break;
      }
      ui.channelNameAt(mi, _show_name, sizeof(_show_name));
      _edit_slot = mi;
      _mode = SHOW_KEY;
      break;
    }
    case 4: {  // Share key (QR) - official meshcore://channel/add URL
      char url[160];
      if (!ui.channelShareUrl(mi, url, sizeof(url))) {
        ui.toast("No key to share", C_YELLOW);
        break;
      }
      ui.openQR(url);
      break;
    }
    case 5: {  // Remove
      if (ui.channelIsPublic(mi)) {
        ui.toast("Can't remove Public", C_YELLOW);
        break;
      }
      ui.channelNameAt(mi, _show_name, sizeof(_show_name));
      _edit_slot = mi;
      _mode = CONFIRM_DEL;
      break;
    }
  }
}

void ChannelsScreen::applyEdit() {
  _edit[_elen] = 0;

  if (_mode == ADD_NAME) {
    // Keep a leading '#' for hashtag channels; only strip spaces.
    char* p = _edit;
    while (*p == ' ') p++;
    if (!*p) return;  // stay in field
    StrHelper::strncpy(_newname, p, sizeof(_newname));
    _mode = ADD_KEY;
    _edit[0] = 0;
    _elen = 0;
    return;
  }

  if (_mode == ADD_KEY) {
    // Remember if we will generate a key the user must save
    const bool will_gen = !_edit[0] && _newname[0] != '#';
    int mi = ui.addChannelNamed(_newname, _edit);
    if (mi >= 0) {
      refreshCount();
      // Select the new slot in the occupied list
      _sel = 0;
      for (int i = 0; i < _n; i++) {
        if (ui.channelSlotAt(i) == mi) { _sel = i; break; }
      }
      if (will_gen &&
          ui.channelKeyBase64(mi, _show_key, sizeof(_show_key))) {
        ui.channelNameAt(mi, _show_name, sizeof(_show_name));
        _edit_slot = mi;
        _mode = SHOW_KEY;
      } else {
        _mode = LIST;
      }
    }
    return;
  }

  if (_mode == RENAME) {
    char* p = _edit;
    while (*p == ' ') p++;
    if (!*p) return;
    if (ui.renameChannel(_edit_slot, p)) {
      _mode = LIST;
      refreshCount();
    }
    return;
  }

  if (_mode == REKEY) {
    if (ui.rekeyChannel(_edit_slot, _edit)) {
      _mode = LIST;
      // offer to show the new key when it was random (blank input)
      if (!_edit[0] && ui.channelKeyBase64(_edit_slot, _show_key, sizeof(_show_key))) {
        ui.channelNameAt(_edit_slot, _show_name, sizeof(_show_name));
        _mode = SHOW_KEY;
      }
    }
    return;
  }
}

bool ChannelsScreen::key(uint8_t k) {
  if (_mode == MENU) {
    if (k == 0x0D) { runAction(_menu); return true; }
    if (k == 0x1B || k == 0x08 || k == 0x7F) { _mode = LIST; return true; }
    return true;
  }

  if (_mode == SHOW_KEY) {
    if (k == 0x0D || k == 0x1B || k == 0x08 || k == 0x7F) {
      _mode = LIST;
      return true;
    }
    return true;
  }

  if (_mode == CONFIRM_DEL) {
    if (k == 0x0D) {
      if (ui.removeChannel(_edit_slot)) {
        refreshCount();
        if (_sel >= _n && _sel > 0) _sel--;
      }
      _mode = LIST;
      return true;
    }
    if (k == 0x1B || k == 0x08 || k == 0x7F) { _mode = LIST; return true; }
    return true;
  }

  if (_mode == ADD_NAME || _mode == ADD_KEY || _mode == RENAME || _mode == REKEY) {
    if (k == 0x0D) { applyEdit(); return true; }
    if (k == 0x08 || k == 0x7F) {
      if (_elen > 0) {
        _elen--;
        _edit[_elen] = 0;
      } else if (_mode == ADD_KEY) {
        _mode = ADD_NAME;
        StrHelper::strncpy(_edit, _newname, sizeof(_edit));
        _elen = (int)strlen(_edit);
      } else {
        _mode = LIST;
      }
      return true;
    }
    if (k >= 32 && k < 127 && _elen < (int)sizeof(_edit) - 2) {
      _edit[_elen++] = (char)k;
      _edit[_elen] = 0;
      return true;
    }
    return true;
  }

  // LIST
  if (k == 0x0D) {
    if (_sel >= _n) beginAdd();
    else openMenu();
    return true;
  }
  return false;
}

bool ChannelsScreen::nav(NavEvent e) {
  if (_mode == MENU) {
    if (e == NAV_UP) {
      if (_menu > 0) _menu--;
      return true;
    }
    if (e == NAV_DOWN) {
      if (_menu < N_CH_ACTIONS - 1) _menu++;
      return true;
    }
    if (e == NAV_SELECT) { runAction(_menu); return true; }
    if (e == NAV_BACK) { _mode = LIST; return true; }
    return true;
  }

  if (_mode == SHOW_KEY) {
    if (e == NAV_SELECT || e == NAV_BACK) { _mode = LIST; return true; }
    return true;
  }

  if (_mode == CONFIRM_DEL) {
    if (e == NAV_SELECT) {
      if (ui.removeChannel(_edit_slot)) {
        refreshCount();
        if (_sel >= _n && _sel > 0) _sel--;
      }
      _mode = LIST;
      return true;
    }
    if (e == NAV_BACK) { _mode = LIST; return true; }
    return true;
  }

  if (_mode == ADD_NAME || _mode == ADD_KEY || _mode == RENAME || _mode == REKEY) {
    if (e == NAV_SELECT) { applyEdit(); return true; }
    if (e == NAV_BACK) {
      if (_elen > 0) {
        _elen = 0;
        _edit[0] = 0;
      } else if (_mode == ADD_KEY) {
        _mode = ADD_NAME;
        StrHelper::strncpy(_edit, _newname, sizeof(_edit));
        _elen = (int)strlen(_edit);
      } else {
        _mode = LIST;
      }
      return true;
    }
    if (e == NAV_UP && !ui.symShift()) ui.toggleSym();
    if (e == NAV_DOWN && ui.symShift()) ui.toggleSym();
    return true;
  }

  // LIST
  switch (e) {
    case NAV_UP:
      if (_sel > 0) _sel--;
      return true;
    case NAV_DOWN:
      if (_sel < _n) _sel++;
      return true;
    case NAV_SELECT:
      if (_sel >= _n) beginAdd();
      else openMenu();
      return true;
    default:
      return false;
  }
}

bool ChannelsScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;

  if (_mode == CONFIRM_DEL) {
    const int bw = 280, bh = 100;
    const int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;
    // Remove button
    if (e.x >= bx + 20 && e.x < bx + 120 && e.y >= by + 70 && e.y < by + 92) {
      if (ui.removeChannel(_edit_slot)) {
        refreshCount();
        if (_sel >= _n && _sel > 0) _sel--;
      }
      _mode = LIST;
      return true;
    }
    // Cancel
    if (e.x >= bx + 150 && e.x < bx + 250 && e.y >= by + 70 && e.y < by + 92) {
      _mode = LIST;
      return true;
    }
    return true;
  }

  if (_mode == SHOW_KEY) {
    _mode = LIST;
    return true;
  }

  if (_mode == MENU) {
    int mw = 150, mh = N_CH_ACTIONS * 16 + 12;
    int mx = SCREEN_W - mw - 10;
    int row_y = CH_TOP + (_sel - _top) * CH_ROW_H;
    int my = row_y;
    if (my + mh > SCREEN_H - 6) my = SCREEN_H - 6 - mh;
    if (my < STATUS_H + 4) my = STATUS_H + 4;
    if (e.x >= mx && e.x < mx + mw && e.y >= my && e.y < my + mh) {
      int i = (e.y - my - 5) / 16;
      if (i >= 0 && i < N_CH_ACTIONS) runAction(i);
      return true;
    }
    _mode = LIST;
    return true;
  }

  if (_mode != LIST) return false;
  if (e.y < CH_TOP) return false;
  int r = _top + (e.y - CH_TOP) / CH_ROW_H;
  if (r < 0 || r > _n) return false;
  _sel = r;
  if (_sel >= _n) beginAdd();
  else openMenu();
  return true;
}
