#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/TxtDataHelpers.h>

#define TAB_H      16
#define INPUT_H    22
#define CHAT_TOP   (STATUS_H + 1 + TAB_H)
#define CHAT_BOT   (SCREEN_H - INPUT_H)
#define BUB_MAX_W  230

// quick canned messages - open with a click on an empty compose bar
static const char* CANNED[] = {
  "On my way", "OK", "Received", "Yes", "No",
  "At location", "Call me", "SOS - need help",
};
#define N_CANNED 8

void ChatScreen::enter() {
  // make sure the Public channel thread exists
  ChannelDetails ch;
  if (ui.mesh && ui.mesh->getChannel(0, ch)) {
    ui.store.forChannel(0, ch.name);
  }
  ui.store.sortByRecent(_order);
  _norder = ui.store.numThreads();

  // jump to a requested thread (from contacts screen / notifications)
  int want = ui.pendingThread();
  if (want >= 0) {
    for (int i = 0; i < _norder; i++) {
      if (_order[i] == want) { _tab = i; break; }
    }
    ui.clearPendingThread();
  }
  if (_tab >= _norder) _tab = 0;
  _scroll = 0;
  DeckThread* t = cur();
  if (t) ui.store.markRead(t);
}

DeckThread* ChatScreen::cur() {
  if (_norder == 0) return nullptr;
  return ui.store.thread(_order[_tab]);
}

void ChatScreen::switchTab(int dir) {
  if (_norder == 0) return;
  _tab = (_tab + dir + _norder) % _norder;
  _scroll = 0;
  DeckThread* t = cur();
  if (t) ui.store.markRead(t);
}

void ChatScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Chat");

  // refresh ordering (new threads can appear at any time)
  int prev_thread = _norder ? _order[_tab] : -1;
  ui.store.sortByRecent(_order);
  _norder = ui.store.numThreads();
  if (prev_thread >= 0) {
    for (int i = 0; i < _norder; i++) if (_order[i] == prev_thread) { _tab = i; break; }
  }
  if (_tab >= _norder) _tab = 0;

  // ---- tab bar ----
  c.fillRect(0, STATUS_H + 1, SCREEN_W, TAB_H, C_BG);
  int tx = 4;
  c.setTextSize(1);
  for (int i = 0; i < _norder && tx < SCREEN_W - 20; i++) {
    DeckThread* t = ui.store.thread(_order[i]);
    char label[20];
    char nm[16];
    ellipsize(nm, sizeof(nm), t->title);
    snprintf(label, sizeof(label), "%s%s", t->kind == TK_CHANNEL ? "#" : "", nm);
    int w = strlen(label) * 6 + 12;
    bool selt = i == _tab;
    if (selt) {
      c.fillRoundRect(tx, STATUS_H + 2, w, TAB_H - 3, 4, C_ACCENT_DK);
      c.drawRoundRect(tx, STATUS_H + 2, w, TAB_H - 3, 4, C_ACCENT);
    }
    c.setTextColor(selt ? C_FG : C_FG_DIM);
    c.setCursor(tx + 6, STATUS_H + 5);
    c.print(label);
    if (t->unread > 0 && !selt) {
      c.fillCircle(tx + w - 3, STATUS_H + 4, 3, C_RED);
    }
    tx += w + 4;
  }

  DeckThread* t = cur();
  _nhits = 0;

  // ---- messages (rendered bottom-up) ----
  if (!t || t->count == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(70, 120);
    c.print(t ? "No messages yet - say hi!" : "No conversations yet");
  } else {
    int y = CHAT_BOT - 4 + _scroll;    // bottom edge of newest bubble
    for (int i = t->count - 1; i >= 0 && y > CHAT_TOP - 60; i--) {
      DeckMsg* m = ui.store.msgAt(t, i);
      if (!m) continue;
      bool out = m->flags & MF_OUT;
      bool show_name = t->kind == TK_CHANNEL && !out;

      int text_h = measureRichTextHeight(c, BUB_MAX_W - 14, m->text, 1);
      int bub_h = text_h + 8 + (show_name ? 10 : 0);
      int bub_w = BUB_MAX_W;
      // shrink narrow messages
      int longest = 0, cur_len = 0;
      for (const char* p = m->text; ; p++) {
        if (*p == '\n' || *p == 0) { if (cur_len > longest) longest = cur_len; cur_len = 0; if (!*p) break; }
        else cur_len++;
      }
      int want_w = longest * 6 + 18;
      int name_w = show_name ? (int)strlen(m->sender) * 6 + 50 : 0;
      if (want_w < name_w) want_w = name_w;
      if (want_w < bub_w) bub_w = want_w;
      if (bub_w < 40) bub_w = 40;

      int by = y - bub_h;
      int bx = out ? SCREEN_W - bub_w - 6 : 6;

      c.fillRoundRect(bx, by, bub_w, bub_h, 7, out ? C_BUB_OUT : C_BUB_IN);

      int ty2 = by + 4;
      if (show_name) {
        c.setTextSize(1);
        c.setTextColor(nameColor(m->sender));
        c.setCursor(bx + 7, ty2);
        c.print(m->sender);
        // signal hint next to name
        char sig[16];
        snprintf(sig, sizeof(sig), "%s", m->hops == 0xFF ? "direct" : "");
        if (m->hops != 0xFF) snprintf(sig, sizeof(sig), "%dhop", m->hops);
        c.setTextColor(C_FG_FAINT);
        c.setCursor(bx + bub_w - strlen(sig) * 6 - 6, ty2);
        c.print(sig);
        ty2 += 10;
      }
      drawRichText(c, bx + 7, ty2, bub_w - 14, m->text, out ? C_BUB_OUT_TXT : C_FG, 1);

      // time + delivery ticks under bubble
      char meta[24];
      char ago[8];
      ui.fmtAgo(ago, sizeof(ago), m->ts);
      snprintf(meta, sizeof(meta), "%s", ago);
      c.setTextSize(1);
      c.setTextColor(C_FG_FAINT);
      int meta_y = y - 1;
      (void)meta_y;
      if (out) {
        const char* tick = (m->flags & MF_DELIVERED) ? "\xFB\xFB" : (m->flags & MF_FAILED) ? "x" : "...";
        uint16_t tc = (m->flags & MF_DELIVERED) ? C_GREEN : (m->flags & MF_FAILED) ? C_RED : C_FG_FAINT;
        c.setCursor(bx - strlen(ago) * 6 - 16 - 8, by + bub_h - 8);
        c.print(ago);
        c.setTextColor(tc);
        c.setCursor(bx - 14, by + bub_h - 8);
        if ((m->flags & MF_DELIVERED)) {
          // double tick drawn manually
          c.drawLine(bx - 14, by + bub_h - 5, bx - 12, by + bub_h - 3, tc);
          c.drawLine(bx - 12, by + bub_h - 3, bx - 8, by + bub_h - 8, tc);
          c.drawLine(bx - 10, by + bub_h - 5, bx - 8, by + bub_h - 3, tc);
          c.drawLine(bx - 8, by + bub_h - 3, bx - 4, by + bub_h - 8, tc);
        } else {
          c.print(tick);
        }
      } else {
        c.setCursor(bx + bub_w + 4, by + bub_h - 8);
        c.print(ago);
      }

      if (_nhits < 24) {
        _hits[_nhits].y0 = by;
        _hits[_nhits].y1 = y;
        _hits[_nhits].msg_idx = i;
        _nhits++;
      }
      y = by - 5;
    }
  }

  // top fade line under tabs
  c.drawFastHLine(0, CHAT_TOP - 1, SCREEN_W, C_FG_FAINT);

  // ---- compose bar ----
  c.fillRect(0, CHAT_BOT, SCREEN_W, INPUT_H, C_BG_RAISED);
  c.setTextSize(1);
  c.setTextColor(C_FG);
  char shown[46];
  int maxc = 42;
  if (_clen <= maxc) {
    memcpy(shown, _compose, _clen);
    shown[_clen] = 0;
  } else {
    snprintf(shown, sizeof(shown), "..%s", _compose + _clen - maxc + 2);
  }
  c.setCursor(8, CHAT_BOT + 7);
  if (_clen == 0) {
    c.setTextColor(C_FG_FAINT);
    c.print(t && t->kind == TK_CHANNEL ? "Message  (click=quick msgs)" : "Type a message  (click=quick)");
  } else {
    c.print(shown);
    // cursor
    int cx2 = 8 + strlen(shown) * 6;
    c.fillRect(cx2 + 1, CHAT_BOT + 6, 2, 10, C_ACCENT);
  }

  // ---- canned quick-message picker overlay ----
  if (_canned >= 0) {
    int mw = 180, rh = 20, mh = N_CANNED * rh + 24;
    int mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
    c.fillRoundRect(mx, my, mw, mh, 8, C_BG_RAISED);
    c.drawRoundRect(mx, my, mw, mh, 8, C_ACCENT);
    c.setTextSize(1);
    c.setTextColor(C_ACCENT);
    c.setCursor(mx + 10, my + 7);
    c.print("Quick messages");
    for (int i = 0; i < N_CANNED; i++) {
      int ry = my + 20 + i * rh;
      bool s = i == _canned;
      if (s) c.fillRoundRect(mx + 4, ry - 1, mw - 8, rh - 2, 4, C_ACCENT_DK);
      c.setTextColor(s ? C_FG : C_FG_DIM);
      c.setCursor(mx + 12, ry + 4);
      c.print(CANNED[i]);
    }
  }
}

void ChatScreen::sendCompose() {
  DeckThread* t = cur();
  if (!t || _clen == 0) return;
  _compose[_clen] = 0;
  bool ok;
  if (t->kind == TK_CHANNEL) ok = ui.sendChannel(t->channel_idx, _compose);
  else ok = ui.sendDM(t->pub_prefix, _compose);
  if (ok) {
    _clen = 0;
    _compose[0] = 0;
    _scroll = 0;
    ui.hw.beep(1568, 40);
  }
}

// send one of the canned quick messages, then close the picker
void ChatScreen::sendCanned(int i) {
  _canned = -1;
  if (i < 0 || i >= N_CANNED) return;
  StrHelper::strncpy(_compose, CANNED[i], sizeof(_compose));
  _clen = strlen(_compose);
  sendCompose();
}

bool ChatScreen::key(uint8_t k) {
  // canned quick-message picker open: digits pick, backspace/esc close
  if (_canned >= 0) {
    if (k >= '1' && k <= '0' + N_CANNED) { sendCanned(k - '1'); return true; }
    if (k == 0x0D) { sendCanned(_canned); return true; }
    _canned = -1;   // any other key closes the picker
    return true;
  }
  if (k == 0x0D) {
    if (_clen == 0) { _canned = 0; return true; }   // empty + enter -> quick messages
    sendCompose();
    return true;
  }
  if (k == 0x08) {
    if (_clen > 0) { _clen--; _compose[_clen] = 0; return true; }
    return false;   // empty compose -> back
  }
  if (k == 0x09) { switchTab(1); return true; }   // tab key
  if (k >= 32 && k < 127 && _clen < MD_TEXT_LEN - 2) {
    _compose[_clen++] = k;
    _compose[_clen] = 0;
    return true;
  }
  return false;
}

bool ChatScreen::nav(NavEvent e) {
  // canned quick-message picker navigation
  if (_canned >= 0) {
    switch (e) {
      case NAV_UP:    if (_canned > 0) _canned--; return true;
      case NAV_DOWN:  if (_canned < N_CANNED - 1) _canned++; return true;
      case NAV_SELECT: sendCanned(_canned); return true;
      case NAV_BACK:  _canned = -1; return true;
      default: return true;
    }
  }
  switch (e) {
    case NAV_UP:    _scroll += 24; return true;
    case NAV_DOWN:  _scroll -= 24; if (_scroll < 0) _scroll = 0; return true;
    case NAV_LEFT:  switchTab(-1); return true;
    case NAV_RIGHT: switchTab(1); return true;
    case NAV_SELECT: if (_clen == 0) { _canned = 0; return true; } sendCompose(); return true;
    default: return false;
  }
}

bool ChatScreen::touch(const TouchEvent& e) {
  if (e.kind == TouchEvent::DRAG) {
    _scroll += e.dy;
    if (_scroll < 0) _scroll = 0;
    return true;
  }
  if (e.kind == TouchEvent::TAP) {
    // tab bar tap: cycle
    if (e.y < CHAT_TOP && e.y > STATUS_H) { switchTab(1); return true; }
    // bubble tap: URL -> QR, else quote sender
    DeckThread* t = cur();
    if (!t) return true;
    for (int i = 0; i < _nhits; i++) {
      if (e.y >= _hits[i].y0 && e.y <= _hits[i].y1) {
        DeckMsg* m = ui.store.msgAt(t, _hits[i].msg_idx);
        if (!m) return true;
        const char* url = strstr(m->text, "http");
        if (url) {
          char u[128];
          int n = 0;
          while (url[n] && url[n] != ' ' && url[n] != '\n' && n < 126) { u[n] = url[n]; n++; }
          u[n] = 0;
          ui.openQR(u);
          return true;
        }
        if (!(m->flags & MF_OUT) && m->sender[0] && _clen < MD_TEXT_LEN - 20) {
          int n = snprintf(_compose + _clen, MD_TEXT_LEN - _clen - 1, "@%s ", m->sender);
          _clen += n;
        }
        return true;
      }
    }
    return true;
  }
  return false;
}
