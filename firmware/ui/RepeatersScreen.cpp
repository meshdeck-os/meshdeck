#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

#define RP_ROW_H 28
#define RP_TOP (STATUS_H + 4)
#define RP_FOOTER 28
#define RP_VIS ((SCREEN_H - RP_TOP - RP_FOOTER) / RP_ROW_H)

void RepeatersScreen::enter() {
  rebuild();
  // Only kick auto-login for rooms not already OK this session
  ui.tryAutoLoginRooms();
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
    sanitizeAscii(_names[_n]);  // GFX font is ASCII-only
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

bool RepeatersScreen::consoleIsFor(const uint8_t* prefix6) const {
  if (!prefix6 || !_console_bound) return false;
  return memcmp(_console_prefix, prefix6, 6) == 0;
}

void RepeatersScreen::ensureConsoleFor(const uint8_t* prefix6) {
  if (!prefix6) return;
  if (_console_bound && memcmp(_console_prefix, prefix6, 6) == 0) return;
  memcpy(_console_prefix, prefix6, 6);
  _console_bound = true;
  _cn = 0;  // new peer - don't mix prior console history
}

void RepeatersScreen::onCliResponse(const char* from, const char* text) {
  // Local UI lines (">") only while a console is open
  if (_mode != MODE_CONSOLE) return;
  if (_cn >= 14) {
    memmove(&_clines[0], &_clines[1], sizeof(CLine) * 13);
    _cn = 13;
  }
  StrHelper::strncpy(_clines[_cn].from, from, sizeof(_clines[_cn].from));
  StrHelper::strncpy(_clines[_cn].text, text, sizeof(_clines[_cn].text));
  sanitizeAscii(_clines[_cn].from);
  sanitizeAscii(_clines[_cn].text);
  _cn++;
}

void RepeatersScreen::onPeerLine(const uint8_t* prefix6, const char* from, const char* text) {
  // Only traffic for the peer whose console is open
  if (_mode != MODE_CONSOLE) return;
  if (!consoleIsFor(prefix6)) return;
  onCliResponse(from, text);
}

void RepeatersScreen::replaceWaitingLine(const char* from, const char* text) {
  if (_mode != MODE_CONSOLE) return;
  // Replace last "waiting for reply..." line so the console doesn't look stuck
  for (int i = _cn - 1; i >= 0; i--) {
    if (strstr(_clines[i].text, "waiting for reply") != nullptr) {
      StrHelper::strncpy(_clines[i].from, from, sizeof(_clines[i].from));
      StrHelper::strncpy(_clines[i].text, text, sizeof(_clines[i].text));
      sanitizeAscii(_clines[i].from);
      sanitizeAscii(_clines[i].text);
      return;
    }
  }
  onCliResponse(from, text);
}

void RepeatersScreen::onLoginFinished(const char* name, bool ok) {
  _awaiting_login = false;
  if (_mode != MODE_CONSOLE) return;
  // Only update if this login result is for the console's peer
  if (name && name[0] && _sel < _n) {
    if (strcmp(_names[_sel], name) != 0) return;
  }
  if (ok)
    replaceWaitingLine(name ? name : "sys", "LOGIN OK - session active");
  else
    replaceWaitingLine(name ? name : "sys", "LOGIN FAILED");
}

void RepeatersScreen::openLogin() {
  ContactInfo* ct = selContact();
  if (!ct) return;
  ensureConsoleFor(ct->id.pub_key);
  _mode = MODE_LOGIN;
  _llen = 0;
  _line[0] = 0;
  _show_pwd = false;
  // Auto-login is rooms-only (repeaters: remember password, no boot login)
  _login_auto = (ct->type == ADV_TYPE_ROOM);
  // Prefill saved password
  if (const UITask::RoomCred* e = ui.findRoomCred(ct->id.pub_key)) {
    StrHelper::strncpy(_line, e->password, sizeof(_line));
    _llen = (int)strlen(_line);
    if (ct->type == ADV_TYPE_ROOM)
      _login_auto = e->auto_login != 0;
    else
      _login_auto = false;
  }
}

void RepeatersScreen::submitLogin() {
  ContactInfo* ct = selContact();
  if (!ct) { _mode = MODE_LIST; return; }
  _line[_llen] = 0;
  // Empty password is allowed (some rooms use blank).
  // Never request boot auto-login for repeaters.
  const bool want_auto = (ct->type == ADV_TYPE_ROOM) && _login_auto;
  if (ui.beginRoomLogin(*ct, _line, want_auto)) {
    // Leave password screen so user sees console / wait state (not silent stuck)
    ensureConsoleFor(ct->id.pub_key);
    _mode = MODE_CONSOLE;
    _awaiting_login = true;
    onCliResponse(">", "login sent...");
    onCliResponse(">", "waiting for reply...");
    // If already connected, beginRoomLogin returns true without a wait
    if (ui.mesh && ui.mesh->isLoggedInto(ct->id.pub_key)) {
      _awaiting_login = false;
      onCliResponse(">", "already in session");
    }
  } else {
    // beginRoomLogin already toasted (send fail / busy). Stay on login with
    // password preserved so user can edit and retry.
    onCliResponse(">", "login not sent - see toast");
  }
  // Keep typed password for retry on failure; only clear on success path
  if (_mode == MODE_CONSOLE) {
    _llen = 0;
    _line[0] = 0;
  }
}

void RepeatersScreen::openConsole() {
  if (!_n) return;
  ContactInfo* ct = selContact();
  if (ct) ensureConsoleFor(ct->id.pub_key);
  else if (_sel < _n) ensureConsoleFor(_prefixes[_sel]);
  _mode = MODE_CONSOLE;
  _llen = 0;
  _line[0] = 0;
}

void RepeatersScreen::toggleAuto() {
  ContactInfo* ct = selContact();
  if (!ct) return;
  if (ct->type != ADV_TYPE_ROOM) {
    ui.toast("Auto-login is for rooms only", C_YELLOW);
    return;
  }
  const UITask::RoomCred* e = ui.findRoomCred(ct->id.pub_key);
  if (!e) {
    ui.toast("Login once first to save", C_YELLOW);
    return;
  }
  bool next = !e->auto_login;
  if (!ui.setRoomAutoLogin(ct->id.pub_key, next)) {
    ui.toast("Auto-login is for rooms only", C_YELLOW);
    return;
  }
  char msg[40];
  snprintf(msg, sizeof(msg), "Auto-login %s", next ? "ON" : "OFF");
  ui.toast(msg, next ? C_GREEN : C_YELLOW);
}

void RepeatersScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  c.setTextSize(1);

  // -- Login dialog ------------------------------------------
  if (_mode == MODE_LOGIN) {
    ContactInfo* ct = selContact();
    char title[40];
    snprintf(title, sizeof(title), "Login: %s", ct ? ct->name : "?");
    ui.drawStatusBar(title);

    c.setTextColor(C_FG_DIM);
    c.setCursor(12, STATUS_H + 16);
    c.print(ct && ct->type == ADV_TYPE_ROOM ? "Room password" : "Repeater password");

    // Password field
    c.fillRoundRect(10, STATUS_H + 36, SCREEN_W - 20, 28, 6, C_BG_RAISED);
    c.drawRoundRect(10, STATUS_H + 36, SCREEN_W - 20, 28, 6, C_ACCENT);
    c.setCursor(18, STATUS_H + 46);
    if (_llen == 0) {
      c.setTextColor(C_FG_FAINT);
      c.print(_show_pwd ? "(type password)" : "(type password, * hidden)");
    } else {
      c.setTextColor(C_FG);
      if (_show_pwd) {
        _line[_llen] = 0;
        c.print(_line);
      } else {
        for (int i = 0; i < _llen; i++) c.write('*');
      }
    }
    // caret
    int cx = 18 + (_llen * 6);
    c.fillRect(cx, STATUS_H + 44, 2, 12, C_ACCENT);

    // Auto-login toggle (rooms only)
    const bool is_room = ct && ct->type == ADV_TYPE_ROOM;
    c.setTextColor(C_FG_DIM);
    c.setCursor(12, STATUS_H + 80);
    c.print("After login");
    if (is_room) {
      c.setTextColor(_login_auto ? C_GREEN : C_FG_FAINT);
      c.setCursor(12, STATUS_H + 96);
      c.printf("[%c] Remember & auto-login on boot", _login_auto ? 'x' : ' ');
    } else {
      c.setTextColor(C_FG_FAINT);
      c.setCursor(12, STATUS_H + 96);
      c.print("Password saved for one-tap (no auto)");
    }

    c.setTextColor(C_FG_FAINT);
    c.setCursor(12, STATUS_H + 120);
    c.print("ENTER  login");
    c.setCursor(12, STATUS_H + 134);
    if (is_room)
      c.print("A/TAB  auto-login");
    else
      c.print("(auto-login: rooms only)");
    c.setCursor(12, STATUS_H + 148);
    c.printf("UP  %s password", _show_pwd ? "hide" : "show");
    c.setCursor(12, STATUS_H + 162);
    c.print("BACK  cancel");

    if (const UITask::RoomCred* e = ct ? ui.findRoomCred(ct->id.pub_key) : nullptr) {
      c.setTextColor(C_CYAN);
      c.setCursor(12, SCREEN_H - 16);
      c.print(e->password[0] ? "Saved password loaded" : "Saved (empty password)");
    }
    return;
  }

  // -- Console -----------------------------------------------
  if (_mode == MODE_CONSOLE) {
    char title[44];
    snprintf(title, sizeof(title), "%s", _sel < _n ? _names[_sel] : "?");
    ui.drawStatusBar(title);

    ContactInfo* ct = selContact();
    bool on = ct && (ui.mesh->isLoggedInto(ct->id.pub_key) ||
                     ui.roomSessionOk(ct->id.pub_key));
    bool syncing = ct && ui.isRoomSyncing(ct->id.pub_key);
    c.setTextColor(syncing ? C_YELLOW : (on ? C_GREEN : (_awaiting_login ? C_YELLOW : C_FG_FAINT)));
    c.setCursor(SCREEN_W - 56, 4);
    if (syncing) {
      uint32_t rem = ui.roomSyncRemainingMs(ct->id.pub_key);
      char chip[8];
      snprintf(chip, sizeof(chip), "syn%u", (unsigned)((rem + 999) / 1000));
      c.print(chip);
    } else {
      c.print(on ? "IN" : (_awaiting_login ? "wait" : "out"));
    }

    // Console log: long replies must wrap with real line advance or they
    // paint over the next message (GFX default wrap doesn't move our y).
    const int text_x = 4 + 11 * 6;
    const int text_max_w = SCREEN_W - text_x - 4;
    const int line_h = 10;
    const int log_bot = SCREEN_H - 22;
    c.setTextSize(1);
    c.setTextWrap(false);

    int y = STATUS_H + 6;
    for (int i = 0; i < _cn && y < log_bot; i++) {
      c.setTextColor(C_FG_FAINT);
      c.setCursor(4, y);
      c.print(_clines[i].from);

      // Wrap body text; first line aligns with "from", continuations indent
      const char* p = _clines[i].text;
      int body_y = y;
      const int max_chars = text_max_w / 6;
      if (max_chars < 1) {
        y += line_h;
        continue;
      }
      bool first = true;
      while (*p && body_y + line_h <= log_bot) {
        int n = 0;
        int last_space = -1;
        while (p[n] && n < max_chars) {
          if (p[n] == ' ') last_space = n;
          if (p[n] == '\n') { n++; break; }
          n++;
        }
        int take = n;
        if (p[n] && last_space > 8 && n >= max_chars)
          take = last_space + 1;  // break on word when possible
        if (take <= 0) take = 1;

        int print_n = take;
        if (print_n > 0 && p[print_n - 1] == '\n') print_n--;
        char buf[56];
        if (print_n >= (int)sizeof(buf)) print_n = (int)sizeof(buf) - 1;
        memcpy(buf, p, print_n);
        buf[print_n] = 0;

        c.setTextColor(C_TERM_RX);
        c.setCursor(text_x, body_y);
        c.print(buf);

        body_y += line_h;
        p += take;
        while (*p == ' ') p++;
        first = false;
        (void)first;
      }
      // Next log entry starts below the last wrapped line of this one
      y = body_y + 2;
    }

    c.fillRect(0, SCREEN_H - 20, SCREEN_W, 20, C_BG_RAISED);
    c.setCursor(6, SCREEN_H - 14);
    if (_llen == 0) {
      c.setTextColor(C_FG_FAINT);
      c.print("cmd  |  /l /s /a /n /c /r");
    } else {
      c.setTextColor(C_FG);
      _line[_llen] = 0;
      c.print(_line);
    }
    c.fillRect(6 + _llen * 6 + 1, SCREEN_H - 15, 2, 10, C_ACCENT);
    return;
  }

  // -- List --------------------------------------------------
  char title[40];
  snprintf(title, sizeof(title), "Repeaters & rooms (%d)", _n);
  ui.drawStatusBar(title);

  if (_n == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(40, 110);
    c.print("No repeaters or rooms heard yet");
    c.setCursor(50, 126);
    c.print("Wait for adverts nearby");
    return;
  }
  if (_sel < _top) _top = _sel;
  if (_sel >= _top + RP_VIS) _top = _sel - RP_VIS + 1;

  for (int i = _top; i < _n && i < _top + RP_VIS; i++) {
    int y = RP_TOP + (i - _top) * RP_ROW_H;
    bool sel = i == _sel;
    if (sel) c.fillRoundRect(2, y, SCREEN_W - 4, RP_ROW_H - 2, 5, C_BG_RAISED);
    bool is_room = _types[i] == ADV_TYPE_ROOM;
    c.fillRect(2, y, 3, RP_ROW_H - 2, is_room ? C_PURPLE : C_ORANGE);

    // Leave room on the right for status chips (ON / AUTO / KEY)
    const int chip_w = 78;
    c.setTextColor(sel ? C_FG : C_FG_DIM);
    c.setCursor(12, y + 3);
    {
      char name[22];
      ellipsize(name, sizeof(name), _names[i]);
      c.print(name);
    }

    ContactInfo* live = ui.mesh->lookupContactByPubKey(_prefixes[i], 6);
    bool connected = live && ui.mesh->isLoggedInto(live->id.pub_key);
    bool sess_ok = ui.roomSessionOk(_prefixes[i]);
    const UITask::RoomCred* cred = ui.findRoomCred(_prefixes[i]);
    bool has_key = cred && (cred->password[0] || true);  // any saved cred
    has_key = (cred != nullptr);
    // AUTO chip only for rooms that opted into boot re-login
    bool auto_on = is_room && cred && cred->auto_login;
    char ago[8];
    ui.fmtAgo(ago, sizeof(ago), _last_adv[i]);

    // Two independent facts (ASCII only for GFX font):
    //   1) session now: IN vs out
    //   2) rooms: auto-login preference; repeaters: key saved only
    bool logged_in = connected || sess_ok;
    c.setTextColor(C_FG_FAINT);
    c.setCursor(12, y + 15);
    const char* kind = is_room ? "room" : "rpt";
    if (logged_in && auto_on)
      c.printf("%s | LOGGED IN | auto on", kind);
    else if (logged_in && has_key)
      c.printf("%s | LOGGED IN | key", kind);
    else if (logged_in)
      c.printf("%s | LOGGED IN", kind);
    else if (auto_on)
      c.printf("%s | not in | auto on", kind);
    else if (has_key)
      c.printf("%s | not in | key saved", kind);
    else
      c.printf("%s | not in | last %s", kind, ago);

    // Chips (right -> left). Always show session chip; AUTO only for rooms.
    //   IN   = currently logged in (this boot / keep-alive)
    //   AUTO = room will re-login after reboot
    //   KEY  = password saved (one-tap); not auto for repeaters
    int cx = SCREEN_W - 6;
    if (auto_on) {
      const char* lab = "AUTO";
      int w = (int)strlen(lab) * 6 + 6;
      cx -= w;
      c.fillRoundRect(cx, y + 6, w - 2, 14, 3, C_BG);
      c.drawRoundRect(cx, y + 6, w - 2, 14, 3, C_CYAN);
      c.setTextColor(C_CYAN);
      c.setCursor(cx + 3, y + 9);
      c.print(lab);
      cx -= 3;
    } else if (has_key) {
      const char* lab = "KEY";
      int w = (int)strlen(lab) * 6 + 6;
      cx -= w;
      c.fillRoundRect(cx, y + 6, w - 2, 14, 3, C_BG);
      c.drawRoundRect(cx, y + 6, w - 2, 14, 3, C_YELLOW);
      c.setTextColor(C_YELLOW);
      c.setCursor(cx + 3, y + 9);
      c.print(lab);
      cx -= 3;
    }
    // Session chip always present so IN vs out is never ambiguous
    {
      const char* lab = logged_in ? "IN" : "out";
      uint16_t col = logged_in ? C_GREEN : C_FG_FAINT;
      int w = (int)strlen(lab) * 6 + 6;
      cx -= w;
      c.fillRoundRect(cx, y + 6, w - 2, 14, 3, C_BG);
      c.drawRoundRect(cx, y + 6, w - 2, 14, 3, col);
      c.setTextColor(col);
      c.setCursor(cx + 3, y + 9);
      c.print(lab);
    }
    (void)chip_w;
  }

  c.setTextColor(C_FG_FAINT);
  c.setCursor(4, SCREEN_H - 24);
  c.print("IN=session  AUTO=rooms only  R=resync");
  c.setCursor(4, SCREEN_H - 12);
  c.print("ENTER open  L login  A auto  C console");
}

void RepeatersScreen::resyncSelected(bool full_history) {
  ContactInfo* ct = selContact();
  if (!ct) return;
  if (ct->type != ADV_TYPE_ROOM) {
    ui.toast("Resync is for rooms only", C_YELLOW);
    return;
  }
  if (!ui.findRoomCred(ct->id.pub_key)) {
    ui.toast("Login once first (save password)", C_YELLOW);
    openLogin();
    return;
  }
  ensureConsoleFor(ct->id.pub_key);
  _mode = MODE_CONSOLE;
  _awaiting_login = true;
  onCliResponse(">", full_history ? "resync full backlog..." : "re-login...");
  onCliResponse(">", "waiting for reply...");
  if (!ui.resyncRoom(*ct, full_history)) {
    _awaiting_login = false;
    onCliResponse(">", "resync failed");
  }
}

void RepeatersScreen::sendLine() {
  ContactInfo* ct = selContact();
  if (!ct || _llen == 0) { _llen = 0; return; }
  _line[_llen] = 0;

  // Local slash commands - typed as /l, /s, ... then ENTER so bare letters
  // can start normal words (e.g. "set", "advert", "clock").
  if (_line[0] == '/') {
    const char* p = _line + 1;
    while (*p == ' ') p++;
    char tok[16];
    int ti = 0;
    while (*p && *p != ' ' && ti < (int)sizeof(tok) - 1) {
      char ch = *p++;
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
      tok[ti++] = ch;
    }
    tok[ti] = 0;

    if (tok[0] == 0) { _llen = 0; _line[0] = 0; return; }

    // UI-only actions (not sent to the node)
    if (strcmp(tok, "l") == 0 || strcmp(tok, "login") == 0) {
      onCliResponse(">", "/login");
      _llen = 0; _line[0] = 0;
      openLogin();
      return;
    }
    if (strcmp(tok, "r") == 0 || strcmp(tok, "resync") == 0) {
      onCliResponse(">", "/resync");
      _llen = 0; _line[0] = 0;
      resyncSelected(true);
      return;
    }

    // Expand short forms into the remote CLI command, then send
    const char* expand = nullptr;
    if (strcmp(tok, "s") == 0 || strcmp(tok, "stats") == 0) expand = "stats";
    else if (strcmp(tok, "a") == 0 || strcmp(tok, "advert") == 0) expand = "advert";
    else if (strcmp(tok, "c") == 0 || strcmp(tok, "clock") == 0) expand = "clock sync";
    else if (strcmp(tok, "n") == 0 || strcmp(tok, "neighbors") == 0) expand = "neighbors";

    if (expand) {
      StrHelper::strncpy(_line, expand, sizeof(_line));
      _llen = (int)strlen(_line);
    }
    // Unknown /cmd is left as-is and sent to the node
  }

  // Room backlog sync: any TX to the room contends with stop-and-wait push ACKs
  if (!ui.allowSendToContact(*ct, true)) return;
  uint32_t est_timeout;
  int res = ui.mesh->sendCommandData(*ct, ui.epochNow(), 0, _line, est_timeout);
  if (res == MSG_SEND_FAILED) ui.toast("Send failed", C_RED);
  else {
    onCliResponse(">", _line);
    ui.termLog(C_TERM_TX, "[cmd->%s] %s", ct->name, _line);
  }
  _llen = 0;
  _line[0] = 0;
}

bool RepeatersScreen::key(uint8_t k) {
  // -- Login dialog --
  if (_mode == MODE_LOGIN) {
    if (k == 0x0D) { submitLogin(); return true; }
    if (k == 0x08 || k == 0x7F) {
      if (_llen > 0) { _llen--; return true; }
      _mode = MODE_LIST;
      return true;
    }
    if (k == 0x1B) { _mode = MODE_LIST; _llen = 0; return true; }
    if (k == 'a' || k == 'A') {
      // Rooms only: toggle auto-login when password line empty
      ContactInfo* ct = selContact();
      if (_llen == 0 && ct && ct->type == ADV_TYPE_ROOM) {
        _login_auto = !_login_auto;
        return true;
      }
    }
    // Toggle auto with tab when typing (rooms only; tab rarely in passwords)
    if (k == 0x09) {
      ContactInfo* ct = selContact();
      if (ct && ct->type == ADV_TYPE_ROOM) {
        _login_auto = !_login_auto;
        return true;
      }
    }
    // All printable chars (including '.') go into the password.
    // Show/hide uses trackball UP/DOWN - see nav().
    if (k >= 32 && k < 127 && _llen < 15) {  // MeshCore max 15
      _line[_llen++] = k;
      return true;
    }
    return false;
  }

  // -- Console --
  // No bare-letter shortcuts here - letters type into the command line.
  // Shortcuts are slash commands: /l /s /a /n /c /r then ENTER (see sendLine).
  if (_mode == MODE_CONSOLE) {
    if (k == 0x0D) { sendLine(); return true; }
    if (k == 0x08 || k == 0x7F) {
      if (_llen > 0) { _llen--; return true; }
      _mode = MODE_LIST;
      return true;
    }
    if (k >= 32 && k < 127 && _llen < (int)sizeof(_line) - 2) {
      _line[_llen++] = k;
      return true;
    }
    return false;
  }

  // -- List --
  if (!_n) return false;
  if (k == 0x0D || k == ' ') {
    ContactInfo* ct = selContact();
    if (!ct) return true;
    bool on = ui.mesh && ui.mesh->isLoggedInto(ct->id.pub_key);
    const UITask::RoomCred* e = ui.findRoomCred(ct->id.pub_key);
    if (on) {
      openConsole();
    } else if (e) {
      // One-tap re-login with saved cred (blank password rooms included).
      // Boot auto-login only for rooms - repeaters keep the password only.
      const bool want_auto = (ct->type == ADV_TYPE_ROOM) && e->auto_login;
      if (ui.beginRoomLogin(*ct, e->password, want_auto)) {
        ensureConsoleFor(ct->id.pub_key);
        _mode = MODE_CONSOLE;
        _awaiting_login = true;
        onCliResponse(">", "re-login with saved password...");
        onCliResponse(">", "waiting for reply...");
        if (ui.mesh && ui.mesh->isLoggedInto(ct->id.pub_key)) {
          _awaiting_login = false;
          onCliResponse(">", "already in session");
        }
      } else {
        // Send blocked - open password UI so user can force a manual attempt
        openLogin();
      }
    } else {
      openLogin();
    }
    return true;
  }
  if (k == 'l' || k == 'L') { openLogin(); return true; }
  if (k == 'r' || k == 'R') { resyncSelected(true); return true; }
  if (k == 'a' || k == 'A') { toggleAuto(); return true; }
  if (k == 'c' || k == 'C') { openConsole(); return true; }
  if (k == 'f' || k == 'F') {
    ContactInfo* ct = selContact();
    if (ct && ui.findRoomCred(ct->id.pub_key)) {
      ui.forgetRoomCred(ct->id.pub_key);
      ui.toast("Password forgotten", C_YELLOW);
    }
    return true;
  }
  return false;
}

bool RepeatersScreen::nav(NavEvent e) {
  if (_mode == MODE_LOGIN) {
    if (e == NAV_SELECT) { submitLogin(); return true; }
    if (e == NAV_BACK) { _mode = MODE_LIST; _llen = 0; return true; }
    // Trackball up/down: show/hide password (never steals printable keys)
    if (e == NAV_UP || e == NAV_DOWN) {
      _show_pwd = !_show_pwd;
      return true;
    }
    if (e == NAV_LEFT || e == NAV_RIGHT) {
      ContactInfo* ct = selContact();
      if (ct && ct->type == ADV_TYPE_ROOM) {
        _login_auto = !_login_auto;
        return true;
      }
      return true;
    }
    return false;
  }
  if (_mode == MODE_CONSOLE) {
    if (e == NAV_SELECT) { sendLine(); return true; }
    if (e == NAV_BACK) {
      _mode = MODE_LIST;
      _llen = 0;
      return true;
    }
    return false;
  }
  switch (e) {
    case NAV_UP:   if (_sel > 0) _sel--; return true;
    case NAV_DOWN: if (_sel < _n - 1) _sel++; return true;
    case NAV_SELECT: return key(0x0D);
    default: return false;
  }
}
