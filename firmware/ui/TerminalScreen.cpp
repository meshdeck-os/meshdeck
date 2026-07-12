#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/TxtDataHelpers.h>
#include <esp_heap_caps.h>

#define T_LINE_H 11
#define T_TOP (STATUS_H + 2)
#define T_BOT (SCREEN_H - 18)
#define T_VIS ((T_BOT - T_TOP) / T_LINE_H)

void TerminalScreen::enter() {
  _scroll = 0;
}

void TerminalScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Terminal");
  c.setTextSize(1);

  int total = ui.termCount();
  int newest = total - 1 - _scroll;
  int y = T_BOT - T_LINE_H;
  for (int i = newest; i >= 0 && y >= T_TOP; i--) {
    TermLine* l = ui.termLine(i);
    if (!l) break;
    c.setTextColor(l->color);
    c.setCursor(2, y);
    c.print(l->text);
    y -= T_LINE_H;
  }
  if (_scroll > 0) {
    c.setTextColor(C_YELLOW);
    c.setCursor(SCREEN_W - 40, T_TOP);
    char s[10];
    snprintf(s, sizeof(s), "-%d", _scroll);
    c.print(s);
  }

  // input line
  c.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, C_BG_RAISED);
  c.setCursor(4, SCREEN_H - 12);
  c.setTextColor(C_TERM_IN);
  c.print("> ");
  _line[_llen] = 0;
  c.setTextColor(C_FG);
  // show tail if too long
  if (_llen <= 48) c.print(_line);
  else c.print(_line + _llen - 48);
  int cx = 16 + (_llen > 48 ? 48 : _llen) * 6;
  c.fillRect(cx + 1, SCREEN_H - 13, 2, 10, C_ACCENT);
}

ContactInfo* TerminalScreen::findByName(const char* name) {
  ContactInfo* ct = ui.mesh->searchContactsByPrefix(name);
  return ct;
}

void TerminalScreen::execCommand(const char* line_in) {
  char line[96];
  StrHelper::strncpy(line, line_in, sizeof(line));
  ui.termLog(C_TERM_IN, "> %s", line);

  // split cmd + args
  char* args = strchr(line, ' ');
  if (args) { *args++ = 0; while (*args == ' ') args++; }

  MyMesh* m = ui.mesh;

  if (strcmp(line, "help") == 0) {
    ui.termLog(C_TERM_SYS, "advert | advertf | contacts | heard | noise | mem");
    ui.termLog(C_TERM_SYS, "send <name> <msg>   ch <idx> <msg>");
    ui.termLog(C_TERM_SYS, "login <name> <pwd>  cmd <name> <cli...>  trace <name>");
    ui.termLog(C_TERM_SYS, "freq <MHz> | sf <7-12> | bw <kHz> | cr <5-8> | power <dBm>");
    ui.termLog(C_TERM_SYS, "name <newname> | time <epoch> | clear | reboot");
  } else if (strcmp(line, "advert") == 0) {
    m->advert();
    ui.termLog(C_TERM_TX, "zero-hop advert sent");
  } else if (strcmp(line, "advertf") == 0) {
    m->advertFlood();
    ui.termLog(C_TERM_TX, "flood advert sent");
  } else if (strcmp(line, "contacts") == 0) {
    int n = m->getNumContacts();
    for (int i = 0; i < n; i++) {
      ContactInfo ct;
      if (m->getContactByIdx(i, ct)) {
        ui.termLog(C_TERM_SYS, "%2d. %-24s t%d path %d", i, ct.name, ct.type,
                   ct.out_path_len == 0xFF ? -1 : ct.out_path_len);
      }
    }
    if (n == 0) ui.termLog(C_TERM_SYS, "(none)");
  } else if (strcmp(line, "heard") == 0) {
    for (int i = 0; i < ui.heardCount() && i < 10; i++) {
      const HeardEntry* e = ui.heardAt(i);
      char ago[8];
      ui.fmtAgo(ago, sizeof(ago), e->at);
      ui.termLog(C_TERM_SYS, "%-20s %s ago snr %s", e->name, ago, StrHelper::ftoa(e->snr4 / 4.0f));
    }
  } else if (strcmp(line, "noise") == 0) {
    ui.termLog(C_TERM_SYS, "noise floor: %d dBm", m->getNoiseFloorNow());
  } else if (strcmp(line, "mem") == 0) {
    ui.termLog(C_TERM_SYS, "heap %u free / psram %u free",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  } else if (strcmp(line, "send") == 0 && args) {
    char* msg = strchr(args, ' ');
    if (msg) {
      *msg++ = 0;
      ContactInfo* ct = findByName(args);
      if (ct) ui.sendDM(ct->id.pub_key, msg);
      else ui.termLog(C_TERM_ERR, "no contact matching '%s'", args);
    }
  } else if (strcmp(line, "ch") == 0 && args) {
    char* msg = strchr(args, ' ');
    if (msg) {
      *msg++ = 0;
      ui.sendChannel(atoi(args), msg);
    }
  } else if (strcmp(line, "login") == 0 && args) {
    char* pwd = strchr(args, ' ');
    if (pwd) {
      *pwd++ = 0;
      ContactInfo* ct = findByName(args);
      if (ct) {
        uint32_t est;
        int res = m->sendLogin(*ct, pwd, est);
        ui.termLog(res == MSG_SEND_FAILED ? C_TERM_ERR : C_TERM_TX, "login -> %s", ct->name);
      } else ui.termLog(C_TERM_ERR, "no contact matching '%s'", args);
    }
  } else if (strcmp(line, "cmd") == 0 && args) {
    char* cli = strchr(args, ' ');
    if (cli) {
      *cli++ = 0;
      ContactInfo* ct = findByName(args);
      if (ct) {
        uint32_t est;
        int res = m->sendCommandData(*ct, ui.epochNow(), 0, cli, est);
        ui.termLog(res == MSG_SEND_FAILED ? C_TERM_ERR : C_TERM_TX, "cmd -> %s: %s", ct->name, cli);
      } else ui.termLog(C_TERM_ERR, "no contact matching '%s'", args);
    }
  } else if (strcmp(line, "trace") == 0 && args) {
    ContactInfo* ct = findByName(args);
    if (ct) ui.startTrace(*ct);
    else ui.termLog(C_TERM_ERR, "no contact matching '%s'", args);
  } else if (strcmp(line, "freq") == 0 && args) {
    float f = atof(args);
    if (f > 100 && f < 1000) {
      ui.prefs->freq = f;
      m->applyRadioPrefs();
      m->savePrefs();
      ui.termLog(C_TERM_SYS, "freq set to %s MHz", StrHelper::ftoa(f));
    } else ui.termLog(C_TERM_ERR, "bad freq");
  } else if (strcmp(line, "sf") == 0 && args) {
    int v = atoi(args);
    if (v >= 7 && v <= 12) {
      ui.prefs->sf = v;
      m->applyRadioPrefs();
      m->savePrefs();
      ui.termLog(C_TERM_SYS, "sf set to %d", v);
    } else ui.termLog(C_TERM_ERR, "sf must be 7-12");
  } else if (strcmp(line, "bw") == 0 && args) {
    float v = atof(args);
    if (v >= 7 && v <= 500) {
      ui.prefs->bw = v;
      m->applyRadioPrefs();
      m->savePrefs();
      ui.termLog(C_TERM_SYS, "bw set to %s kHz", StrHelper::ftoa(v));
    } else ui.termLog(C_TERM_ERR, "bad bw");
  } else if (strcmp(line, "cr") == 0 && args) {
    int v = atoi(args);
    if (v >= 5 && v <= 8) {
      ui.prefs->cr = v;
      m->applyRadioPrefs();
      m->savePrefs();
      ui.termLog(C_TERM_SYS, "cr set to %d", v);
    } else ui.termLog(C_TERM_ERR, "cr must be 5-8");
  } else if (strcmp(line, "power") == 0 && args) {
    int v = atoi(args);
    if (v >= 1 && v <= 22) {
      ui.prefs->tx_power_dbm = v;
      m->applyRadioPrefs();
      m->savePrefs();
      ui.termLog(C_TERM_SYS, "tx power set to %d dBm", v);
    } else ui.termLog(C_TERM_ERR, "power must be 1-22");
  } else if (strcmp(line, "name") == 0 && args) {
    StrHelper::strncpy(ui.prefs->node_name, args, sizeof(ui.prefs->node_name));
    m->savePrefs();
    ui.termLog(C_TERM_SYS, "name set to '%s' (advert to announce)", args);
  } else if (strcmp(line, "time") == 0 && args) {
    uint32_t e = strtoul(args, NULL, 10);
    if (e > 1600000000) {
      m->getRTCClock()->setCurrentTime(e);
      ui.termLog(C_TERM_SYS, "clock set");
    } else ui.termLog(C_TERM_ERR, "bad epoch");
  } else if (strcmp(line, "clear") == 0) {
    ui.termClear();
  } else if (strcmp(line, "reboot") == 0) {
    ESP.restart();
  } else if (line[0]) {
    ui.termLog(C_TERM_ERR, "unknown command - try 'help'");
  }
}

bool TerminalScreen::key(uint8_t k) {
  if (k == 0x0D) {
    if (_llen > 0) {
      _line[_llen] = 0;
      _llen = 0;
      _scroll = 0;
      execCommand(_line);
    }
    return true;
  }
  if (k == 0x08) {
    if (_llen > 0) { _llen--; return true; }
    return false;
  }
  if (k >= 32 && k < 127 && _llen < (int)sizeof(_line) - 2) {
    _line[_llen++] = k;
    return true;
  }
  return false;
}

bool TerminalScreen::nav(NavEvent e) {
  switch (e) {
    case NAV_UP:
      if (_scroll < ui.termCount() - 1) _scroll += 3;
      return true;
    case NAV_DOWN:
      _scroll -= 3;
      if (_scroll < 0) _scroll = 0;
      return true;
    default:
      return false;
  }
}
