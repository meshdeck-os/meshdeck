#pragma once
/*
 * All MeshDeck screen classes.
 */
#include "UITask.h"
#include <helpers/ContactInfo.h>

// ---------------------------------------------------------------- Home

class HomeScreen : public Screen {
public:
  HomeScreen(UITask& u) : Screen(u) {}
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  int _sel = 0;
};

// ---------------------------------------------------------------- Chat

class ChatScreen : public Screen {
public:
  ChatScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void sendCompose();
  void sendCanned(int i);
  void switchTab(int dir);
  DeckThread* cur();

  int _tab = 0;              // index into sorted order
  int _order[MD_MAX_THREADS];
  int _norder = 0;
  int _scroll = 0;           // px from bottom
  char _compose[MD_TEXT_LEN];
  int _clen = 0;
  // bubble hitboxes for tap-to-reply / QR
  struct Hit { int16_t y0, y1; int msg_idx; } _hits[24];
  int _nhits = 0;
  int _canned = -1;          // -1 = off, else index into the quick-message list
};

// ---------------------------------------------------------------- Contacts

class ContactsScreen : public Screen {
public:
  ContactsScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void action(int which);
  int _sel = 0;
  int _top = 0;
  int _menu = -1;            // -1 none, else selected action row
};

// ---------------------------------------------------------------- Map

class MapScreen : public Screen {
public:
  MapScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void project(double lat, double lon, int& x, int& y) const;
  void drawNodes();
  double _clat = 54.5, _clon = -3.0;   // UK default centre
  float _scale = 12.0f;                // px per degree lon
  bool _centered_once = false;
};

// ---------------------------------------------------------------- Last heard

class LastHeardScreen : public Screen {
public:
  LastHeardScreen(UITask& u) : Screen(u) {}
  void draw() override;
  bool nav(NavEvent e) override;
private:
  int _top = 0;
};

// ---------------------------------------------------------------- Repeaters

class RepeatersScreen : public Screen {
public:
  RepeatersScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  void onCliResponse(const char* from, const char* text);
private:
  void rebuild();
  void sendLine();
  ContactInfo* selContact();

  uint8_t _prefixes[24][6];
  char _names[24][28];
  uint8_t _types[24];
  uint32_t _last_adv[24];
  int _n = 0;
  int _sel = 0, _top = 0;
  bool _console = false;      // list vs console mode
  char _line[80];
  int _llen = 0;
  bool _pwd_mode = false;
  struct CLine { char from[12]; char text[70]; };
  CLine _clines[14];
  int _cn = 0;
};

// ---------------------------------------------------------------- Trace

class TraceScreen : public Screen {
public:
  TraceScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
private:
  void rebuild();
  uint8_t _prefixes[24][6];
  char _names[24][28];
  uint8_t _hops[24];
  int _n = 0;
  int _sel = 0;
  bool _picking = true;
};

// ---------------------------------------------------------------- Noise floor

class NoiseScreen : public Screen {
public:
  NoiseScreen(UITask& u) : Screen(u) {}
  void draw() override;
};

// ---------------------------------------------------------------- Terminal

class TerminalScreen : public Screen {
public:
  TerminalScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  void execCommand(const char* line);
private:
  ContactInfo* findByName(const char* name);
  char _line[96];
  int _llen = 0;
  int _scroll = 0;   // lines up from newest
};

// ---------------------------------------------------------------- Settings

class SettingsScreen : public Screen {
public:
  SettingsScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
private:
  void adjust(int dir);
  void select();
  void applyEdit();
  int _sel = 0, _top = 0;
  bool _editing = false;
  char _edit[68];          // large enough for a WiFi WPA password
  int _elen = 0;
};

// ---------------------------------------------------------------- QR viewer

class QRScreen : public Screen {
public:
  QRScreen(UITask& u) : Screen(u) {}
  void draw() override;
};

// ---------------------------------------------------------------- Onboarding (first boot)

class OnboardScreen : public Screen {
public:
  OnboardScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void choose(int i);
  int _phase = 0;            // 0 = enter node name, 1 = pick radio preset
  char _name[32];
  int _nlen = 0;
  int _sel = 0, _top = 0;
};

// ---------------------------------------------------------------- Radio diagnostics

class DiagScreen : public Screen {
public:
  DiagScreen(UITask& u) : Screen(u) {}
  void draw() override;
  bool key(uint8_t c) override;
};

// ---------------------------------------------------------------- SOS beacon

class SOSScreen : public Screen {
public:
  SOSScreen(UITask& u) : Screen(u) {}
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
};

// ---------------------------------------------------------------- Map download (WiFi)

#define MAPDL_MAX 128
class MapDownloadScreen : public Screen {
public:
  MapDownloadScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void loadIndex();
  void rebuildFilter();
  void downloadSel();
  char _codes[MAPDL_MAX][8];
  char _names[MAPDL_MAX][28];
  int  _n = 0;               // total regions loaded
  int  _filt[MAPDL_MAX];     // indices matching the filter
  int  _nf = 0;              // number matching
  int  _sel = 0, _top = 0;
  char _filter[20];          // type-to-search text
  int  _flen = 0;
  const char* _status = nullptr;   // e.g. "Connect WiFi first"
};

// ---------------------------------------------------------------- WiFi (join a network)

class WifiScreen : public Screen {
public:
  WifiScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void select();
  void applyEdit();
  void doScan();             // scan nearby networks -> pick list
  void pickSelect();
  int  _sel = 0;             // 0 = SSID, 1 = password, 2 = connect, 3 = remote screen
  bool _editing = false;
  char _edit[68];
  int  _elen = 0;
  // network scan / picker
  bool _picking = false;
  int  _nscan = 0;
  char _scan[16][33];
  int8_t _scan_rssi[16];
  int  _psel = 0, _ptop = 0;
};

// ---------------------------------------------------------------- Channels

class ChannelsScreen : public Screen {
public:
  ChannelsScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
  bool touch(const TouchEvent& e) override;
private:
  void select();
  void applyEdit();
  int  _n = 0;               // channel count (refreshed on enter/draw)
  int  _sel = 0, _top = 0;
  bool _adding = false;
  int  _phase = 0;           // 0 = enter name, 1 = enter key
  char _newname[32];
  char _edit[68];
  int  _elen = 0;
};

// ---------------------------------------------------------------- Voice test (beta)

class VoiceScreen : public Screen {
public:
  VoiceScreen(UITask& u) : Screen(u) {}
  void enter() override;
  void draw() override;
  bool key(uint8_t c) override;
  bool nav(NavEvent e) override;
private:
  void record();
  void playback();
  int16_t* _buf = nullptr;     // PSRAM capture buffer (16 kHz mono, 16-bit)
  int      _cap = 0;           // capacity in samples
  int      _len = 0;           // samples captured
  int      _peak = 0;          // last-capture peak amplitude
  int      _rms = 0;           // last-capture RMS
  bool     _hwok = false;      // ES7210 chip-id read back OK
  char     _status[40] = "hold ENTER to record 2s";
};
