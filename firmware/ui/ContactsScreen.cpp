#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/AdvertDataHelpers.h>
#include <string.h>

#define ROW_H 24
#define FILT_H    18
#define LIST_TOP  (STATUS_H + 1 + FILT_H)
#define VISIBLE ((SCREEN_H - LIST_TOP) / ROW_H)
#ifdef MESHDECK_BETA
static const char* ACTIONS[] = {
  "Message", "Call...", "Show path", "Trace route", "Reset path", "Share (0-hop)", "Remove"
};
// indices: 0 Message, 1 Call, 2 Show path, 3 Trace, 4 Reset, 5 Share, 6 Remove
#else
// No Call... on stable — voice is beta-only
static const char* ACTIONS[] = {
  "Message", "Show path", "Trace route", "Reset path", "Share (0-hop)", "Remove"
};
// indices: 0 Message, 1 Show path, 2 Trace, 3 Reset, 4 Share, 5 Remove
#endif
#define N_ACTIONS ((int)(sizeof(ACTIONS) / sizeof(ACTIONS[0])))



// case-insensitive substring match
static bool nameMatches(const char* name, const char* filt) {
  if (!filt || !filt[0]) return true;
  if (!name) return false;
  // naive case-fold scan
  for (const char* p = name; *p; p++) {
    const char* a = p;
    const char* b = filt;
    while (*a && *b) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
      if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
      if (ca != cb) break;
      a++; b++;
    }
    if (!*b) return true;
  }
  return false;
}

void ContactsScreen::rebuildFilter() {
  _fn = 0;
  if (!ui.mesh) return;
  // Always keep filter NUL-terminated; only apply when user typed something
  if (_flen < 0) _flen = 0;
  if (_flen >= (int)sizeof(_filter)) _flen = (int)sizeof(_filter) - 1;
  _filter[_flen] = 0;

  int n = ui.mesh->getNumContacts();
  const char* filt = (_flen > 0) ? _filter : "";
  for (int i = 0; i < n && _fn < FMAP_MAX; i++) {
    ContactInfo ct;
    if (!ui.mesh->getContactByIdx(i, ct)) continue;
    // Skip transient/anon slots (not real saved contacts)
    if (ct.type == ADV_TYPE_NONE) continue;
    if (nameMatches(ct.name[0] ? ct.name : "?", filt)) {
      _fmap[_fn++] = i;
    }
  }
  if (_sel >= _fn) _sel = _fn ? _fn - 1 : 0;
  if (_sel < 0) _sel = 0;
  if (_top > _sel) _top = _sel;
  if (_top < 0) _top = 0;
}

int ContactsScreen::realIndex(int filtered_i) const {
  if (filtered_i < 0 || filtered_i >= _fn) return -1;
  return _fmap[filtered_i];
}

void ContactsScreen::enter() {
  _menu = -1;
  // Defensive: wipe filter state so we never ghost-filter on first open
  if (_flen <= 0) {
    _flen = 0;
    memset(_filter, 0, sizeof(_filter));
  } else {
    _filter[_flen] = 0;
  }
  _sel = 0;
  _top = 0;
  rebuildFilter();
}

void ContactsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);

  // Rebuild every frame so adds/removes stay correct (cheap for a few hundred contacts)
  rebuildFilter();

  // Title count = listed (excludes anon slots), not raw table size
  char title[40];
  if (_flen)
    snprintf(title, sizeof(title), "Contacts (%d match)", _fn);
  else
    snprintf(title, sizeof(title), "Contacts (%d)", _fn);
  ui.drawStatusBar(title);

  // ---- filter field ----
  c.fillRect(0, STATUS_H + 1, SCREEN_W, FILT_H, C_BG_ALT);
  c.setTextSize(1);
  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, STATUS_H + 6);
  c.print("Filter:");
  c.setTextColor(C_FG);
  c.setCursor(50, STATUS_H + 6);
  if (_flen == 0) {
    c.setTextColor(C_FG_FAINT);
    c.print("(type to search)");
  } else {
    c.print(_filter);
    // cursor
    int cx = 50 + _flen * 6;
    c.fillRect(cx + 1, STATUS_H + 5, 2, 10, C_ACCENT);
  }
  c.drawFastHLine(0, LIST_TOP - 1, SCREEN_W, C_FG_FAINT);

  if (_fn == 0) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(40, 120);
    c.print(_flen > 0 ? "No matches for filter" : "No contacts yet");
    return;
  }

  if (_sel < _top) _top = _sel;
  if (_sel >= _top + VISIBLE) _top = _sel - VISIBLE + 1;

  for (int fi = _top; fi < _fn && fi < _top + VISIBLE; ++fi) {
    ContactInfo ct;
    if (!ui.mesh->getContactByIdx(realIndex(fi), ct)) continue;

    int y = LIST_TOP + (fi - _top) * ROW_H;
    bool sel = (fi == _sel);
    if (sel) c.fillRoundRect(2, y, SCREEN_W - 4, ROW_H - 2, 5, C_BG_RAISED);

    uint16_t tc = ct.type == ADV_TYPE_REPEATER ? C_ORANGE :
                  ct.type == ADV_TYPE_ROOM     ? C_PURPLE :
                  ct.type == ADV_TYPE_SENSOR   ? C_YELLOW : C_CYAN;
    c.fillCircle(12, y + ROW_H / 2 - 1, 5, tc);
    c.setTextColor(C_BG);
    c.setCursor(10, y + ROW_H / 2 - 4);
    c.print(ct.type == ADV_TYPE_REPEATER ? "R" :
            ct.type == ADV_TYPE_ROOM     ? "O" :
            ct.type == ADV_TYPE_SENSOR   ? "S" : "");

    c.setTextColor(sel ? C_FG : C_FG_DIM);
    char nm[26];
    ellipsize(nm, sizeof(nm), ct.name);
    c.setCursor(24, y + 3);
    c.print(nm);

    char info[48];
    char ago[8];
    char path[36];
    ui.fmtAgo(ago, sizeof(ago), ct.lastmod);
    ui.fmtContactPath(path, sizeof(path), ct);
    // Prefer hash path when short enough ("1a > 14"); else hop count
    if (ct.out_path_len != 0xFF && (ct.out_path_len & 63) > 0 &&
        strlen(path) > 0 && strlen(path) <= 22 &&
        strcmp(path, "flood (no path)") != 0 && strcmp(path, "direct") != 0) {
      snprintf(info, sizeof(info), "%s  %s", path, ago);
    } else if (ct.out_path_len == 0xFF) {
      snprintf(info, sizeof(info), "flood  %s", ago);
    } else {
      uint8_t hops = (uint8_t)(ct.out_path_len & 63);
      snprintf(info, sizeof(info), "%u hop%s  %s",
               (unsigned)hops, hops == 1 ? "" : "s", ago);
    }
    c.setTextColor(C_FG_FAINT);
    c.setCursor(24, y + 13);
    c.print(info);

    if (ct.gps_lat || ct.gps_lon) {
      c.setTextColor(C_GREEN);
      c.setCursor(SCREEN_W - 20, y + 8);
      c.print("*");
    }
  }

  if (_fn > VISIBLE) {
    int bar_h = (SCREEN_H - LIST_TOP) * VISIBLE / _fn;
    if (bar_h < 4) bar_h = 4;
    int bar_y = LIST_TOP + (SCREEN_H - LIST_TOP - bar_h) * _top / (_fn - VISIBLE);
    c.fillRect(SCREEN_W - 3, bar_y, 2, bar_h, C_FG_FAINT);
  }

  if (_menu >= 0) {
    int mw = 130, mh = N_ACTIONS * 18 + 10;
    int mx = SCREEN_W - mw - 14, my = 60;
    c.fillRoundRect(mx, my, mw, mh, 8, C_BG_RAISED);
    c.drawRoundRect(mx, my, mw, mh, 8, C_ACCENT);
    for (int i = 0; i < N_ACTIONS; ++i) {
      c.setTextColor(i == _menu ? C_ACCENT : C_FG_DIM);
      c.setCursor(mx + 12, my + 8 + i * 18);
      c.print(ACTIONS[i]);
    }
  }
}

void ContactsScreen::action(int which) {
  int ri = realIndex(_sel);
  if (ri < 0) return;
  ContactInfo ct;
  if (!ui.mesh->getContactByIdx(ri, ct)) return;
  _menu = -1;

  // Indices match ACTIONS[] (Call... only when MESHDECK_BETA)
  switch (which) {
    case 0: {  // Message
      DeckThread* t = ui.store.forContact(ct.id.pub_key, ct.name);
      if (t) ui.openThread(ui.store.indexOf(t));
      break;
    }
#ifdef MESHDECK_BETA
    case 1: {  // Call... (beta: max 1 hop — direct or via one repeater)
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (!live) {
        ui.toast("Contact not found", C_RED);
        break;
      }
      if (!ui.mesh->canVoiceCallContact(*live)) {
        uint8_t hops = MyMesh::voiceHopCount(live->out_path_len);
        char msg[48];
        snprintf(msg, sizeof(msg), "Need <=1 hop (path %u)", (unsigned)hops);
        ui.toast(msg, C_YELLOW);
        break;
      }
      (void)ui.startVoiceCall(*live);
      break;
    }
    case 2:   // Show path
#else
    case 1:   // Show path
#endif
    {
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      const ContactInfo& use = live ? *live : ct;
      char path[64];
      ui.fmtContactPath(path, sizeof(path), use);
      char hops[16];
      if (use.out_path_len == 0xFF) strcpy(hops, "flood");
      else {
        uint8_t n = (uint8_t)(use.out_path_len & 63);
        snprintf(hops, sizeof(hops), "%u hop%s", (unsigned)n, n == 1 ? "" : "s");
      }
      char msg[72];
      snprintf(msg, sizeof(msg), "%s: %s", hops, path);
      ui.toast(msg, C_CYAN);
      ui.termLog(C_TERM_SYS, "path %s -> %s", use.name, path);
      break;
    }
#ifdef MESHDECK_BETA
    case 3:   // Trace route
#else
    case 2:   // Trace route
#endif
    {
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (live && ui.startTrace(*live)) ui.go(SCR_TRACE);
      break;
    }
#ifdef MESHDECK_BETA
    case 4:
#else
    case 3:
#endif
    {
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (live) {
        ui.mesh->resetPathTo(*live);
        ui.toast("Path reset - next send floods");
      }
      break;
    }
#ifdef MESHDECK_BETA
    case 5:
#else
    case 4:
#endif
    {
      ui.mesh->shareContactZeroHop(ct);
      ui.toast("Contact shared 0-hop");
      break;
    }
#ifdef MESHDECK_BETA
    case 6:
#else
    case 5:
#endif
    {
      ContactInfo* live = ui.mesh->lookupContactByPubKey(ct.id.pub_key, 6);
      if (live && ui.mesh->removeContact(*live)) {
        ui.mesh->saveContacts();  // persist removal
        ui.toast("Contact removed", C_YELLOW);
        rebuildFilter();
        if (_sel >= _fn && _sel > 0) _sel--;
      }
      break;
    }
  }
}

bool ContactsScreen::key(uint8_t k) {
  if (_menu >= 0) {
    if (k == 0x0D) { action(_menu); return true; }
    return false;
  }

  if (k == 0x0D) {                 // Enter → action menu
    if (_fn) _menu = 0;
    return true;
  }
  if (k == 0x08 || k == 0x7F) {    // Backspace
    if (_flen > 0) {
      _filter[--_flen] = 0;
      _sel = 0; _top = 0;
      rebuildFilter();
      return true;
    }
    return false;                  // empty filter → back
  }
  if (k == 0x1B) {                 // Esc clears filter
    if (_flen) {
      _flen = 0; _filter[0] = 0;
      _sel = 0; _top = 0;
      rebuildFilter();
      return true;
    }
    return false;
  }
  if (k == 'm' && _flen == 0) { action(0); return true; }  // Message
#ifdef MESHDECK_BETA
  if (k == 'c' && _flen == 0) { action(1); return true; }  // Call...
  if (k == 'p' && _flen == 0) { action(2); return true; }  // Show path
  if (k == 't' && _flen == 0) { action(3); return true; }  // Trace
#else
  if (k == 'p' && _flen == 0) { action(1); return true; }  // Show path
  if (k == 't' && _flen == 0) { action(2); return true; }  // Trace
#endif

  // Printable → filter text
  if (k >= 32 && k < 127 && _flen < (int)sizeof(_filter) - 1) {
    _filter[_flen++] = (char)k;
    _filter[_flen] = 0;
    _sel = 0; _top = 0;
    rebuildFilter();
    return true;
  }
  return false;
}

bool ContactsScreen::nav(NavEvent e) {
  if (_menu >= 0) {
    switch (e) {
      case NAV_UP:     if (_menu > 0) _menu--; return true;
      case NAV_DOWN:   if (_menu < N_ACTIONS - 1) _menu++; return true;
      case NAV_SELECT: action(_menu); return true;
      case NAV_BACK:   _menu = -1; return true;
      default: return true;
    }
  }
  switch (e) {
    case NAV_UP:     if (_sel > 0) _sel--; return true;
    case NAV_DOWN:   if (_sel < _fn - 1) _sel++; return true;
    case NAV_SELECT: if (_fn) _menu = 0; return true;
    case NAV_BACK:
      if (_flen) {
        _flen = 0; _filter[0] = 0;
        _sel = 0; _top = 0;
        rebuildFilter();
        return true;
      }
      return false;
    default: return false;
  }
}

bool ContactsScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  if (_menu >= 0) {
    _menu = -1;
    return true;
  }
  // idx is filtered list row, not raw contact index
  int idx = _top + (e.y - LIST_TOP) / ROW_H;
  if (idx >= 0 && idx < _fn) {
    if (idx == _sel) _menu = 0;
    else _sel = idx;
  }
  return true;
}
