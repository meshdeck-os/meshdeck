#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ContactInfo.h>
#include <ctype.h>

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

// MeshCore apps share contacts in chat as:
//   <publickeyhex:type:Name>
// or with spaces: <hex… :1:NOBBY-KM7GYW>
// Also support: meshcore://contact/add?name=…&public_key=…&type=…
struct ParsedContactShare {
  uint8_t pub[32];
  int     pub_bytes;   // 6..32
  uint8_t type;
  char    name[32];
  bool    ok;
};

static bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Parse hex (ignoring internal spaces) into out[]; return byte count or -1
static int parseHexBytes(const char* s, int slen, uint8_t* out, int out_max) {
  int ni = 0;
  int hi = -1;
  for (int i = 0; i < slen; i++) {
    if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t') continue;
    int n = hexNibble(s[i]);
    if (n < 0) return -1;
    if (hi < 0) hi = n;
    else {
      if (ni >= out_max) return -1;
      out[ni++] = (uint8_t)((hi << 4) | n);
      hi = -1;
    }
  }
  if (hi >= 0) return -1;  // odd nibble
  return ni;
}

static bool parseAngleContactShare(const char* text, ParsedContactShare& out) {
  out.ok = false;
  out.pub_bytes = 0;
  out.type = ADV_TYPE_CHAT;
  out.name[0] = 0;
  if (!text) return false;

  const char* p = text;
  while ((p = strchr(p, '<')) != nullptr) {
    p++;  // after '<'
    // Find closing '>'
    const char* end = strchr(p, '>');
    if (!end) return false;
    int inner_len = (int)(end - p);
    if (inner_len < 5) { p = end + 1; continue; }

    // Find the *last two* colons so name can contain ':' rarely;
    // format is hex : type : name
    const char* c2 = nullptr;
    const char* c1 = nullptr;
    for (const char* q = end - 1; q > p; q--) {
      if (*q == ':') {
        if (!c2) c2 = q;
        else { c1 = q; break; }
      }
    }
    if (!c1 || !c2 || c2 <= c1 + 1) { p = end + 1; continue; }

    // Hex part: p .. c1
    int hex_len = (int)(c1 - p);
    // trim trailing spaces in hex region
    while (hex_len > 0 && (p[hex_len - 1] == ' ' || p[hex_len - 1] == '\t'))
      hex_len--;
    // trim leading spaces
    while (hex_len > 0 && (*p == ' ' || *p == '\t')) { p++; hex_len--; }

    int nbytes = parseHexBytes(p, hex_len, out.pub, 32);
    if (nbytes < 6) { p = end + 1; continue; }  // need at least 6-byte prefix
    out.pub_bytes = nbytes;

    // Type: c1+1 .. c2
    int t = 0;
    bool any = false;
    for (const char* q = c1 + 1; q < c2; q++) {
      if (*q == ' ') continue;
      if (*q < '0' || *q > '9') { t = -1; break; }
      t = t * 10 + (*q - '0');
      any = true;
    }
    if (!any || t < 0 || t > 15) { p = end + 1; continue; }
    out.type = (uint8_t)t;
    if (out.type == ADV_TYPE_NONE) out.type = ADV_TYPE_CHAT;

    // Name: c2+1 .. end
    const char* np = c2 + 1;
    while (np < end && (*np == ' ' || *np == '\t')) np++;
    int nlen = (int)(end - np);
    while (nlen > 0 && (np[nlen - 1] == ' ' || np[nlen - 1] == '\t')) nlen--;
    if (nlen <= 0 || nlen >= (int)sizeof(out.name)) { p = end + 1; continue; }
    memcpy(out.name, np, nlen);
    out.name[nlen] = 0;
    // Sanitize name to printable ASCII for GFX / ContactInfo
    for (int i = 0; out.name[i]; i++) {
      if ((unsigned char)out.name[i] < 32 || (unsigned char)out.name[i] > 126)
        out.name[i] = '?';
    }
    out.ok = true;
    return true;
  }
  return false;
}

static bool parseUrlContactShare(const char* text, ParsedContactShare& out) {
  out.ok = false;
  out.pub_bytes = 0;
  out.type = ADV_TYPE_CHAT;
  out.name[0] = 0;
  if (!text) return false;
  const char* u = strstr(text, "meshcore://contact/add?");
  if (!u) u = strstr(text, "meshcore://contact/add?");
  if (!u) return false;
  u = strchr(u, '?');
  if (!u) return false;
  u++;

  char pk_hex[80] = {0};
  char name_enc[64] = {0};
  int type = 1;

  // crude query parse
  const char* p = u;
  while (*p && *p != ' ' && *p != '\n') {
    const char* amp = strchr(p, '&');
    const char* end = amp ? amp : p + strlen(p);
    // also stop at whitespace
    for (const char* s = p; s < end; s++)
      if (*s == ' ' || *s == '\n') { end = s; break; }
    if (strncmp(p, "public_key=", 11) == 0) {
      int n = (int)(end - (p + 11));
      if (n > 0 && n < (int)sizeof(pk_hex)) {
        memcpy(pk_hex, p + 11, n);
        pk_hex[n] = 0;
      }
    } else if (strncmp(p, "name=", 5) == 0) {
      int n = (int)(end - (p + 5));
      if (n > 0 && n < (int)sizeof(name_enc)) {
        memcpy(name_enc, p + 5, n);
        name_enc[n] = 0;
      }
    } else if (strncmp(p, "type=", 5) == 0) {
      type = atoi(p + 5);
    }
    if (!amp) break;
    p = amp + 1;
  }

  int nbytes = parseHexBytes(pk_hex, (int)strlen(pk_hex), out.pub, 32);
  if (nbytes < 6) return false;
  out.pub_bytes = nbytes;
  out.type = (uint8_t)((type >= 1 && type <= 4) ? type : ADV_TYPE_CHAT);

  // URL-decode name (+ and %XX minimal)
  int oi = 0;
  for (int i = 0; name_enc[i] && oi < (int)sizeof(out.name) - 1; i++) {
    if (name_enc[i] == '+') out.name[oi++] = ' ';
    else if (name_enc[i] == '%' && isHex(name_enc[i + 1]) && isHex(name_enc[i + 2])) {
      out.name[oi++] = (char)((hexNibble(name_enc[i + 1]) << 4) | hexNibble(name_enc[i + 2]));
      i += 2;
    } else out.name[oi++] = name_enc[i];
  }
  out.name[oi] = 0;
  if (!out.name[0]) strcpy(out.name, "Contact");
  out.ok = true;
  return true;
}

static bool findContactShare(const char* text, ParsedContactShare& out) {
  if (parseAngleContactShare(text, out)) return true;
  if (parseUrlContactShare(text, out)) return true;
  return false;
}

static bool messageHasContactShare(const char* text) {
  ParsedContactShare tmp;
  return findContactShare(text, tmp);
}

void ChatScreen::enter() {
  _add_dlg = false;
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
  if (t) {
    ui.store.markRead(t);
    ui.reresolveThreadSenders(t);   // <-- add this
  }
}

bool ChatScreen::tryOfferContactShare(const char* text) {
  ParsedContactShare sh;
  if (!findContactShare(text, sh) || !ui.mesh) return false;

  // Prefer a full ContactInfo from recent adverts when we only have a prefix
  ContactInfo* recent = ui.findRecentContact(sh.pub);  // matches 6-byte prefix
  if (sh.pub_bytes < (int)PUB_KEY_SIZE) {
    if (!recent) {
      ContactInfo* live = ui.mesh->lookupContactByPubKey(sh.pub, sh.pub_bytes);
      if (live) {
        ui.toast("Already in contacts", C_YELLOW);
        return true;
      }
      ui.toast("Need full key or advert first", C_YELLOW);
      return true;  // handled (cannot add without full key)
    }
    memcpy(_add_pub, recent->id.pub_key, PUB_KEY_SIZE);
    _add_type = recent->type ? recent->type : sh.type;
    if (sh.name[0]) StrHelper::strncpy(_add_name, sh.name, sizeof(_add_name));
    else StrHelper::strncpy(_add_name, recent->name, sizeof(_add_name));
  } else {
    ContactInfo* live = ui.mesh->lookupContactByPubKey(sh.pub, PUB_KEY_SIZE);
    if (live) {
      ui.toast("Already in contacts", C_YELLOW);
      return true;
    }
    memcpy(_add_pub, sh.pub, PUB_KEY_SIZE);
    _add_type = sh.type;
    StrHelper::strncpy(_add_name, sh.name, sizeof(_add_name));
  }
  if (!_add_name[0]) strcpy(_add_name, "Contact");
  _add_dlg = true;
  return true;
}

bool ChatScreen::confirmAddContact() {
  if (!ui.mesh) return false;
  ContactInfo ci;
  memset(&ci, 0, sizeof(ci));
  memcpy(ci.id.pub_key, _add_pub, PUB_KEY_SIZE);
  StrHelper::strncpy(ci.name, _add_name, sizeof(ci.name));
  ci.type = _add_type ? _add_type : ADV_TYPE_CHAT;
  ci.out_path_len = OUT_PATH_UNKNOWN;
  ci.lastmod = ui.epochNow();
  ci.shared_secret_valid = false;

  // If we have a richer recent advert (path, GPS), prefer that
  ContactInfo* recent = ui.findRecentContact(_add_pub);
  if (recent && memcmp(recent->id.pub_key, _add_pub, PUB_KEY_SIZE) == 0) {
    ci = *recent;
    if (_add_name[0]) StrHelper::strncpy(ci.name, _add_name, sizeof(ci.name));
    ci.shared_secret_valid = false;
  }

  if (ui.mesh->lookupContactByPubKey(ci.id.pub_key, PUB_KEY_SIZE)) {
    ui.toast("Already in contacts", C_YELLOW);
    _add_dlg = false;
    return true;
  }
  if (ui.mesh->addContact(ci)) {
    ui.mesh->saveContacts();
    char msg[48];
    snprintf(msg, sizeof(msg), "Added %s", ci.name);
    ui.toast(msg, C_GREEN);
    ui.termLog(C_TERM_SYS, "contact added from share: %s type=%u",
               ci.name, (unsigned)ci.type);
  } else {
    ui.toast("Contact list full", C_RED);
  }
  _add_dlg = false;
  return true;
}

void ChatScreen::drawAddContactDialog() {
  GFXcanvas16& c = ui.cv();
  // Dim backdrop
  for (int y = 0; y < SCREEN_H; y += 2)
    c.drawFastHLine(0, y, SCREEN_W, C_BG);

  const int bw = 280, bh = 110;
  const int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;
  c.fillRoundRect(bx, by, bw, bh, 8, C_BG_RAISED);
  c.drawRoundRect(bx, by, bw, bh, 8, C_ACCENT);

  c.setTextSize(1);
  c.setTextColor(C_FG);
  c.setCursor(bx + 14, by + 14);
  c.print("Add contact?");

  c.setTextColor(C_CYAN);
  c.setCursor(bx + 14, by + 34);
  {
    char line[40];
    ellipsize(line, sizeof(line), _add_name);
    c.print(line);
  }

  c.setTextColor(C_FG_FAINT);
  c.setCursor(bx + 14, by + 50);
  const char* kind =
      _add_type == ADV_TYPE_REPEATER ? "repeater" :
      _add_type == ADV_TYPE_ROOM     ? "room" :
      _add_type == ADV_TYPE_SENSOR   ? "sensor" : "companion";
  c.printf("%s  %02X%02X%02X%02X…", kind,
           _add_pub[0], _add_pub[1], _add_pub[2], _add_pub[3]);

  // Buttons
  c.fillRoundRect(bx + 20, by + 72, 100, 24, 5, C_GREEN);
  c.fillRoundRect(bx + 150, by + 72, 100, 24, 5, C_BG_ALT);
  c.drawRoundRect(bx + 150, by + 72, 100, 24, 5, C_FG_DIM);
  c.setTextColor(C_BG);
  c.setCursor(bx + 48, by + 80);
  c.print("Add");
  c.setTextColor(C_FG);
  c.setCursor(bx + 170, by + 80);
  c.print("Cancel");
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

  // Keep the current tab stable while threads are re-ordered
  int prev_thread = _norder ? _order[_tab] : -1;
  ui.store.sortByRecent(_order);
  _norder = ui.store.numThreads();
  if (prev_thread >= 0) {
    for (int i = 0; i < _norder; i++) {
      if (_order[i] == prev_thread) { _tab = i; break; }
    }
  }
  if (_tab >= _norder) _tab = 0;

  DeckThread* t = cur();
  _nhits = 0;

  // ---- messages first (bottom-up), constrained to [CHAT_TOP, CHAT_BOT) ----
  if (!t || t->count == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(70, (CHAT_TOP + CHAT_BOT) / 2);
    c.print(t ? "No messages yet - say hi!" : "No conversations yet");
  } else {
    int y = CHAT_BOT - 4 + _scroll;
    bool dbg_once = true;

    for (int i = t->count - 1; i >= 0 && y > CHAT_TOP; i--) {
      DeckMsg* m = ui.store.msgAt(t, i);
      if (!m) continue;

      const bool out = (m->flags & MF_OUT) != 0;
      const bool show_name = !out && m->sender[0];

      // Truncate name early so width math is accurate
      char namebuf[18];
      namebuf[0] = 0;
      if (show_name) {
        StrHelper::strncpy(namebuf, m->sender, sizeof(namebuf));
        if (strlen(namebuf) > 14) {
          namebuf[13] = '.';
          namebuf[14] = '.';
          namebuf[15] = 0;
        }
      }

      // Name-line meta (hops + SNR) — build before width calc
      char sig[24];
      sig[0] = 0;
      if (show_name) {
        if (m->hops == 0xFF || m->hops == 0) {
          strcpy(sig, "direct");
        } else if (m->hops < 32) {
          snprintf(sig, sizeof(sig), "%uhop", (unsigned)m->hops);
        } else {
          strcpy(sig, "?");
        }
        if (m->snr4 != 0) {
          char s2[12];
          snprintf(s2, sizeof(s2), " %ddB", (int)m->snr4 / 4);
          strncat(sig, s2, sizeof(sig) - strlen(sig) - 1);
        }
      }

      // Width first — height must use the *same* wrap width as drawRichText,
      // or multi-line text overflows into the next bubble.
      int bub_w = BUB_MAX_W;

      // Message text width (raw char count; word-wrap may still add lines)
      int longest = 0, cur_len = 0;
      for (const char* p = m->text; ; p++) {
        if (*p == '\n' || *p == 0) {
          if (cur_len > longest) longest = cur_len;
          cur_len = 0;
          if (!*p) break;
        } else {
          cur_len++;
        }
      }
      int want_w = longest * 6 + 18;

      // Name row needs: pad + name + gap + sig + pad
      if (show_name) {
        int name_row_w = 7 + (int)strlen(namebuf) * 6 + 8 + (int)strlen(sig) * 6 + 7;
        if (want_w < name_row_w) want_w = name_row_w;
      }

      if (want_w < bub_w) bub_w = want_w;
      if (bub_w < 40) bub_w = 40;
      if (bub_w > BUB_MAX_W) bub_w = BUB_MAX_W;

      const int text_max_w = bub_w - 14;
      const int text_h = measureRichTextHeight(c, text_max_w, m->text, 1);
      const bool has_share = messageHasContactShare(m->text);
      // top pad 4 + text + bottom pad 4; name row + optional "tap to add" chip
      int bub_h = text_h + 8 + (show_name ? 10 : 0) + (has_share ? 12 : 0);
      // Extra gap so wrapped last lines never kiss the next bubble
      if (bub_h < 18) bub_h = 18;

      const int by = y - bub_h;

      // Entirely above the chat window — stop
      if (by + bub_h <= CHAT_TOP) break;

      // Straddles the top edge — skip so we never paint on the tabs
      if (by < CHAT_TOP) {
        y = by - 5;
        continue;
      }

      const int bx = out ? (SCREEN_W - bub_w - 6) : 6;

      c.fillRoundRect(bx, by, bub_w, bub_h, 7, out ? C_BUB_OUT : C_BUB_IN);
      if (has_share)
        c.drawRoundRect(bx, by, bub_w, bub_h, 7, C_CYAN);

      int ty = by + 4;
      if (show_name) {
        c.setTextSize(1);
        c.setTextColor(nameColor(m->sender));
        c.setCursor(bx + 7, ty);
        c.print(namebuf);

        c.setTextColor(C_FG_FAINT);
        int rw = (int)strlen(sig) * 6;
        c.setCursor(bx + bub_w - rw - 6, ty);
        c.print(sig);

        ty += 10;
      }

      // Contact shares: draw body in accent so it looks tappable
      const int used_h = drawRichText(c, bx + 7, ty, text_max_w, m->text,
                                      has_share ? C_CYAN :
                                      (out ? C_BUB_OUT_TXT : C_FG), 1);
      (void)used_h;
      if (has_share && bub_h >= 28) {
        c.setTextColor(C_ACCENT);
        c.setCursor(bx + 7, by + bub_h - 10);
        c.print("tap to add");
      }

      // Relative time under every bubble
      char ago[10];
      ui.fmtAgo(ago, sizeof(ago), m->ts);

      if (dbg_once) {
        static uint32_t last_dbg_ms = 0;
        if (millis() - last_dbg_ms > 1500) {
          ui.termLog(C_TERM_SYS,
                     "draw kind=%u out=%d ts=%lu ago='%s' hops=%u snr4=%d bub_w=%d sender='%s'",
                     (unsigned)t->kind,
                     out ? 1 : 0,
                     (unsigned long)m->ts,
                     ago,
                     (unsigned)m->hops,
                     (int)m->snr4,
                     bub_w,
                     m->sender);
          last_dbg_ms = millis();
        }
        dbg_once = false;
      }

      c.setTextSize(1);
      c.setTextColor(C_FG_FAINT);

      if (out) {
        c.setCursor(bx - (int)strlen(ago) * 6 - 18, by + bub_h - 8);
        c.print(ago);

        const uint16_t tc =
            (m->flags & MF_DELIVERED) ? C_GREEN :
            (m->flags & MF_FAILED)    ? C_RED   : C_FG_FAINT;
        c.setTextColor(tc);
        c.setCursor(bx - 14, by + bub_h - 8);
        if (m->flags & MF_DELIVERED) {
          c.drawLine(bx - 14, by + bub_h - 5, bx - 12, by + bub_h - 3, tc);
          c.drawLine(bx - 12, by + bub_h - 3, bx -  8, by + bub_h - 8, tc);
          c.drawLine(bx - 10, by + bub_h - 5, bx -  8, by + bub_h - 3, tc);
          c.drawLine(bx -  8, by + bub_h - 3, bx -  4, by + bub_h - 8, tc);
        } else if (m->flags & MF_FAILED) {
          c.print("x");
        } else {
          c.print("...");
        }
      } else {
        c.setCursor(bx + bub_w + 4, by + bub_h - 8);
        c.print(ago);
      }

      if (_nhits < 24) {
        _hits[_nhits].y0 = by;
        _hits[_nhits].y1 = y;
        _hits[_nhits].msg_idx = i;
        _hits[_nhits].has_share = has_share ? 1 : 0;
        _nhits++;
      }

      y = by - 8;  // spacing between bubbles
    }
  }

  // ---- tab bar drawn LAST so messages can never cover it ----
  c.fillRect(0, STATUS_H + 1, SCREEN_W, TAB_H, C_BG);
  {
    int tx = 4;
    c.setTextSize(1);
    for (int i = 0; i < _norder && tx < SCREEN_W - 20; i++) {
      DeckThread* tt = ui.store.thread(_order[i]);
      if (!tt) continue;

      char nm[16];
      ellipsize(nm, sizeof(nm), tt->title);
      char label[20];
      snprintf(label, sizeof(label), "%s%s",
               tt->kind == TK_CHANNEL ? "#" : "", nm);

      const int w = (int)strlen(label) * 6 + 12;
      const bool selt = (i == _tab);

      if (selt) {
        c.fillRoundRect(tx, STATUS_H + 2, w, TAB_H - 3, 4, C_ACCENT_DK);
        c.drawRoundRect(tx, STATUS_H + 2, w, TAB_H - 3, 4, C_ACCENT);
      }
      c.setTextColor(selt ? C_FG : C_FG_DIM);
      c.setCursor(tx + 6, STATUS_H + 5);
      c.print(label);

      if (tt->unread > 0 && !selt) {
        c.fillCircle(tx + w - 3, STATUS_H + 4, 3, C_RED);
      }
      tx += w + 4;
    }
  }
  c.drawFastHLine(0, CHAT_TOP - 1, SCREEN_W, C_FG_FAINT);

  // ---- compose bar ----
  c.fillRect(0, CHAT_BOT, SCREEN_W, INPUT_H, C_BG_RAISED);
  c.setTextSize(1);

  char shown[46];
  const int maxc = 42;
  if (_clen <= maxc) {
    memcpy(shown, _compose, _clen);
    shown[_clen] = 0;
  } else {
    snprintf(shown, sizeof(shown), "..%s", _compose + _clen - maxc + 2);
  }

  c.setCursor(8, CHAT_BOT + 7);
  // Room backlog: block compose hint while syncing
  bool room_sync = false;
  uint32_t sync_rem = 0;
  if (t && t->kind == TK_CONTACT && ui.mesh) {
    ContactInfo* live = ui.mesh->lookupContactByPubKey(t->pub_prefix, 6);
    if (live && live->type == ADV_TYPE_ROOM && ui.isRoomSyncing(live->id.pub_key)) {
      room_sync = true;
      sync_rem = ui.roomSyncRemainingMs(live->id.pub_key);
    }
  }
  if (room_sync) {
    c.setTextColor(C_YELLOW);
    char hint[44];
    snprintf(hint, sizeof(hint), "Syncing room... %us",
             (unsigned)((sync_rem + 999) / 1000));
    c.print(hint);
  } else if (_clen == 0) {
    c.setTextColor(C_FG_FAINT);
    c.print(t && t->kind == TK_CHANNEL
                ? "Message  (click=quick msgs)"
                : "Type a message  (click=quick)");
  } else {
    c.setTextColor(C_FG);
    c.print(shown);
    const int cx = 8 + (int)strlen(shown) * 6;
    c.fillRect(cx + 1, CHAT_BOT + 6, 2, 10, C_ACCENT);
  }

  // ---- canned quick-message picker ----
  if (_canned >= 0) {
    const int mw = 180, rh = 20, mh = N_CANNED * rh + 24;
    const int mx = (SCREEN_W - mw) / 2;
    const int my = (SCREEN_H - mh) / 2;

    c.fillRoundRect(mx, my, mw, mh, 8, C_BG_RAISED);
    c.drawRoundRect(mx, my, mw, mh, 8, C_ACCENT);

    c.setTextSize(1);
    c.setTextColor(C_ACCENT);
    c.setCursor(mx + 10, my + 7);
    c.print("Quick messages");

    for (int i = 0; i < N_CANNED; i++) {
      const int ry = my + 20 + i * rh;
      const bool s = (i == _canned);
      if (s) c.fillRoundRect(mx + 4, ry - 1, mw - 8, rh - 2, 4, C_ACCENT_DK);
      c.setTextColor(s ? C_FG : C_FG_DIM);
      c.setCursor(mx + 12, ry + 4);
      c.print(CANNED[i]);
    }
  }

  if (_add_dlg) drawAddContactDialog();
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
  // Add-contact confirm dialog
  if (_add_dlg) {
    if (k == 0x0D || k == 'y' || k == 'Y' || k == 'a' || k == 'A') {
      confirmAddContact();
      return true;
    }
    if (k == 0x1B || k == 0x08 || k == 0x7F || k == 'n' || k == 'N') {
      _add_dlg = false;
      return true;
    }
    return true;  // swallow keys while dialog open
  }
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
  if (k == 0x08 || k == 0x7F) {   // backspace OR delete (T-Deck sends 0x7F)
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
  if (_add_dlg) {
    if (e == NAV_SELECT) { confirmAddContact(); return true; }
    if (e == NAV_BACK || e == NAV_LEFT) { _add_dlg = false; return true; }
    return true;
  }
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
  if (_add_dlg) {
    if (e.kind != TouchEvent::TAP) return true;
    // Dialog buttons: Add left, Cancel right (see drawAddContactDialog)
    const int bw = 280, bh = 110;
    const int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;
    if (e.y >= by + 72 && e.y <= by + 96) {
      if (e.x >= bx + 20 && e.x <= bx + 120) {
        confirmAddContact();
        return true;
      }
      if (e.x >= bx + 150 && e.x <= bx + 250) {
        _add_dlg = false;
        return true;
      }
    }
    // Tap outside cancels
    if (e.x < bx || e.x > bx + bw || e.y < by || e.y > by + bh)
      _add_dlg = false;
    return true;
  }
  if (e.kind == TouchEvent::DRAG) {
    _scroll += e.dy;
    if (_scroll < 0) _scroll = 0;
    return true;
  }
  if (e.kind == TouchEvent::TAP) {
    // tab bar tap: cycle
    if (e.y < CHAT_TOP && e.y > STATUS_H) { switchTab(1); return true; }
    // bubble tap: contact share -> confirm add; URL -> QR; else quote sender
    DeckThread* t = cur();
    if (!t) return true;
    for (int i = 0; i < _nhits; i++) {
      if (e.y >= _hits[i].y0 && e.y <= _hits[i].y1) {
        DeckMsg* m = ui.store.msgAt(t, _hits[i].msg_idx);
        if (!m) return true;
        // tap a failed (red x) outgoing message to resend it
        if ((m->flags & MF_OUT) && (m->flags & MF_FAILED)) {
          if (t->kind == TK_CONTACT) ui.sendDM(t->pub_prefix, m->text);
          else ui.sendChannel(t->channel_idx, m->text);
          ui.toast("Resending message", C_ACCENT);
          return true;
        }
        // Shared contact card (room / channel / DM) — offer to add
        if (tryOfferContactShare(m->text))
          return true;
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
