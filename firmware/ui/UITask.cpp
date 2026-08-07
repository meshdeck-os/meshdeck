#include "UITask.h"
#include "../MyMesh.h"
#include "AllScreens.h"
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <SPIFFS.h>
#include <RTClib.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <time.h>
#include <HTTPClient.h>
#include <SD.h>

#define SETTINGS_FILE "/meshdeck_set.bin"
#define SD_SETTINGS_FILE "/meshdeck/config.bin"   // SD backup, survives a flash wipe (#12)
#define ROOM_CRED_FILE "/meshdeck_rooms.bin"
#define ROOM_CRED_MAGIC 0x524D4331u  // "RMC1"

// ---------------------------------------------------------------- text utils
// Built-in GFX font: glyphs for 0x20..0x7E only. UTF-8 / · / – / … print as junk.

void sanitizeAscii(char* s) {
  if (!s) return;
  char* w = s;
  const unsigned char* r = (const unsigned char*)s;
  while (*r) {
    unsigned char c = *r++;
    if (c >= 0x20 && c <= 0x7E) {
      *w++ = (char)c;
    } else if (c >= 0xC0) {
      // Skip rest of UTF-8 sequence, emit one '?'
      if ((c & 0xE0) == 0xC0) { if (*r) r++; }
      else if ((c & 0xF0) == 0xE0) { if (*r) r++; if (*r) r++; }
      else if ((c & 0xF8) == 0xF0) { if (*r) r++; if (*r) r++; if (*r) r++; }
      *w++ = '?';
    } else if (c == '\n' || c == '\t') {
      *w++ = ' ';
    }
    // drop other controls
  }
  *w = 0;
}

void ellipsize(char* dst, size_t dst_sz, const char* src) {
  if (!dst || dst_sz == 0) return;
  if (!src) { dst[0] = 0; return; }
  // Sanitize into dst first (printable ASCII only)
  size_t wi = 0;
  const unsigned char* r = (const unsigned char*)src;
  while (*r && wi + 1 < dst_sz) {
    unsigned char c = *r++;
    if (c >= 0x20 && c <= 0x7E) {
      dst[wi++] = (char)c;
    } else if (c >= 0xC0) {
      if ((c & 0xE0) == 0xC0) { if (*r) r++; }
      else if ((c & 0xF0) == 0xE0) { if (*r) r++; if (*r) r++; }
      else if ((c & 0xF8) == 0xF0) { if (*r) r++; if (*r) r++; if (*r) r++; }
      if (wi + 1 < dst_sz) dst[wi++] = '?';
    } else if (c == '\n' || c == '\t') {
      if (wi + 1 < dst_sz) dst[wi++] = ' ';
    }
  }
  dst[wi] = 0;
  // Ellipsize if we filled the buffer (source still had more)
  if (*r && dst_sz >= 4) {
    size_t keep = dst_sz - 4;  // room for "..." + NUL
    dst[keep] = '.';
    dst[keep + 1] = '.';
    dst[keep + 2] = '.';
    dst[keep + 3] = 0;
  } else if (wi >= dst_sz - 1 && dst_sz >= 4) {
    // truncated by buffer; add ellipsis
    dst[dst_sz - 4] = '.';
    dst[dst_sz - 3] = '.';
    dst[dst_sz - 2] = '.';
    dst[dst_sz - 1] = 0;
  }
}

// tiny emoji glyphs, drawn procedurally (12x12)
struct EmojiDef { const char* utf8; uint8_t kind; };
static const EmojiDef EMOJIS[] = {
  { "\xF0\x9F\x98\x80", 0 },  // grinning
  { "\xF0\x9F\x98\x82", 1 },  // joy
  { "\xF0\x9F\x98\x89", 2 },  // wink
  { "\xF0\x9F\x98\xA2", 3 },  // cry
  { "\xE2\x9D\xA4",     4 },  // heart
  { "\xF0\x9F\x91\x8D", 5 },  // thumbs up
  { "\xF0\x9F\x94\xA5", 6 },  // fire
  { "\xF0\x9F\x98\xAE", 7 },  // wow
};

static void drawEmoji(GFXcanvas16& cv, int x, int y, uint8_t kind) {
  int cxx = x + 6, cy = y + 6;
  switch (kind) {
    case 4:   // heart
      cv.fillCircle(cxx - 3, cy - 2, 3, C_RED);
      cv.fillCircle(cxx + 3, cy - 2, 3, C_RED);
      cv.fillTriangle(cxx - 6, cy - 1, cxx + 6, cy - 1, cxx, cy + 6, C_RED);
      return;
    case 5:   // thumbs up
      cv.fillRect(x + 1, cy, 4, 6, C_YELLOW);
      cv.fillRect(x + 5, cy - 3, 6, 9, C_YELLOW);
      cv.fillRect(x + 5, cy - 6, 3, 4, C_YELLOW);
      return;
    case 6:   // fire
      cv.fillTriangle(cxx, y, cxx - 5, y + 10, cxx + 5, y + 10, C_ORANGE);
      cv.fillTriangle(cxx, y + 4, cxx - 3, y + 11, cxx + 3, y + 11, C_YELLOW);
      return;
    default: break;
  }
  // face variants
  cv.fillCircle(cxx, cy, 6, C_YELLOW);
  cv.fillCircle(cxx - 2, cy - 2, 1, 0x0000);
  if (kind == 2) cv.drawFastHLine(cxx + 1, cy - 2, 3, 0x0000);   // wink
  else cv.fillCircle(cxx + 2, cy - 2, 1, 0x0000);
  if (kind == 0 || kind == 1) {  // smile
    cv.drawFastHLine(cxx - 3, cy + 2, 7, 0x0000);
    cv.drawFastHLine(cxx - 2, cy + 3, 5, 0x0000);
  } else if (kind == 3) {        // cry
    cv.drawFastHLine(cxx - 2, cy + 3, 5, 0x0000);
    cv.fillRect(cxx - 4, cy - 1, 2, 4, C_ACCENT);
  } else if (kind == 7) {        // wow
    cv.drawCircle(cxx, cy + 2, 2, 0x0000);
  } else {
    cv.drawFastHLine(cxx - 2, cy + 2, 5, 0x0000);
  }
}

static int matchEmoji(const char* p, uint8_t& kind) {
  for (auto& e : EMOJIS) {
    size_t l = strlen(e.utf8);
    if (strncmp(p, e.utf8, l) == 0) { kind = e.kind; return (int)l; }
  }
  return 0;
}

// draws wrapped text with emoji support; returns pixel height used
static int richTextInternal(GFXcanvas16* cv, int x, int y, int max_w, const char* text,
                            uint16_t color, int ts, bool render) {
  int cw = 6 * ts, ch = 8 * ts;
  int line_h = ch + 2;
  int cx = 0, cy = 0;
  const char* p = text;
  if (cv && render) {
    cv->setTextSize(ts);
    cv->setTextColor(color);
    cv->setTextWrap(false);
  }
  while (*p) {
    if (*p == '\n') { cx = 0; cy += line_h; p++; continue; }
    uint8_t kind;
    int el = matchEmoji(p, kind);
    if (el > 0) {
      if (cx + 13 > max_w) { cx = 0; cy += line_h; }
      if (render && cv) drawEmoji(*cv, x + cx, y + cy + (ch > 12 ? (ch - 12) / 2 : 0) - 1, kind);
      cx += 14;
      p += el;
      continue;
    }
    if ((uint8_t)*p >= 0x80) {   // other UTF-8: skip continuation bytes, print block
      p++;
      while ((uint8_t)*p >= 0x80 && ((uint8_t)*p & 0xC0) == 0x80) p++;
      if (cx + cw > max_w) { cx = 0; cy += line_h; }
      if (render && cv) { cv->setCursor(x + cx, y + cy); cv->write('?'); }
      cx += cw;
      continue;
    }
    // measure next word for wrap decision
    if (*p == ' ') {
      if (cx != 0) cx += cw;
      p++;
      continue;
    }
    int wlen = 0;
    const char* q = p;
    while (*q && *q != ' ' && *q != '\n' && (uint8_t)*q < 0x80 && !matchEmoji(q, kind)) { wlen++; q++; }
    int wpx = wlen * cw;
    if (cx != 0 && cx + wpx > max_w && wpx <= max_w) { cx = 0; cy += line_h; }
    for (int i = 0; i < wlen; i++) {
      if (cx + cw > max_w) { cx = 0; cy += line_h; }
      if (render && cv) { cv->setCursor(x + cx, y + cy); cv->write(p[i]); }
      cx += cw;
    }
    p += wlen;
  }
  return cy + line_h;
}

int drawRichText(GFXcanvas16& cv, int x, int y, int max_w, const char* text,
                 uint16_t color, int ts) {
  return richTextInternal(&cv, x, y, max_w, text, color, ts, true);
}

int measureRichTextHeight(GFXcanvas16& cv, int max_w, const char* text, int ts) {
  return richTextInternal(nullptr, 0, 0, max_w, text, 0, ts, false);
}

// ---------------------------------------------------------------- UITask

UITask::UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
    : AbstractUITask(board, serial) {
  memset(_screens, 0, sizeof(_screens));
  memset(&set, 0, sizeof(set));
  memset(&trace, 0, sizeof(trace));
  _toast[0] = 0;
  _qr_url[0] = 0;
}

void UITask::earlyInit() {
  // load settings early (SPIFFS not up yet -> defaults; re-loaded in begin())
  set.magic = DECKSET_MAGIC;
  set.brightness = 255;
  set.timeout_s = 60;         // sleep the screen after 60s idle (saves battery; #4)
  set.sounds = 1;
  set.volume = 6;
  set.flip = 0;
  set.always_on = 0;
  set.man_lat = set.man_lon = 0;
  set.touch_map = 2;          // correct T-Deck landscape mapping (swap XY + mirror)
  set.room_login_tries = AUTO_LOGIN_TRIES_DEFAULT;

  hw.begin(false);

  GFXcanvas16& c = cv();
  c.fillScreen(C_BG);
  c.setTextColor(C_ACCENT);
  c.setTextSize(4);
  c.setCursor(58, 92);
  c.print("MeshDeck");
  c.setTextSize(1);
  c.setTextColor(C_FG_DIM);
  c.setCursor(96, 132);
  c.print("MeshCore for T-Deck  v" MESHDECK_VERSION);
  c.setCursor(122, 150);
  c.print("starting radio...");
  hw.push();

  _term = (TermLine*)heap_caps_malloc(sizeof(TermLine) * TERM_LINES, MALLOC_CAP_SPIRAM);
  if (!_term) _term = (TermLine*)malloc(sizeof(TermLine) * TERM_LINES);
  memset(_noise, -128, sizeof(_noise));
}

void UITask::bootStatus(const char* msg) {
  GFXcanvas16& c = cv();
  c.fillRect(0, 144, SCREEN_W, 16, C_BG);
  c.setTextSize(1);
  c.setTextColor(C_FG_DIM);
  c.setCursor((SCREEN_W - (int)strlen(msg) * 6) / 2, 150);
  c.print(msg);
  hw.push();
  Serial.println(msg);
}

void UITask::fatalError(const char* msg) {
  GFXcanvas16& c = cv();
  c.fillScreen(C_BG);
  c.setTextSize(2);
  c.setTextColor(C_RED);
  c.setCursor(20, 100);
  c.print("ERROR: ");
  c.print(msg);
  hw.push();
}

void UITask::begin(MyMesh* m, SensorManager* s, NodePrefs* p) {
  mesh = m;
  sensors = s;
  prefs = p;

  // load persisted settings now that SPIFFS is mounted
  bool loaded = false;
  File f = SPIFFS.open(SETTINGS_FILE, "r");
  if (f) {
    DeckSettings tmp;
    if (f.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == DECKSET_MAGIC) { set = tmp; loaded = true; }
    f.close();
  }
  // If SPIFFS held no valid config (fresh flash, or wiped by a Launcher reflash)
  // restore from the SD backup and re-seed SPIFFS, so the first-boot wizard is
  // skipped and the node comes back exactly as it was. (#12)
  if (!loaded && hw.sdBegin()) {
    File s = SD.open(SD_SETTINGS_FILE, "r");
    if (s) {
      DeckSettings tmp;
      if (s.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == DECKSET_MAGIC) { set = tmp; loaded = true; }
      s.close();
    }
    hw.sdEnd();
    if (loaded) {
      File w = SPIFFS.open(SETTINGS_FILE, "w");
      if (w) { w.write((uint8_t*)&set, sizeof(set)); w.close(); }
    }
  }
  // Migrate old saves off the wrong touch mapping: map 0 (landscape-direct) is
  // never correct on the T-Deck GT911, so treat a stored 0 as "use the default".
  if (set.touch_map == 0) set.touch_map = 2;
  applySettings();

  // WiFi: reconnect to the saved network on boot (T-Deck Plus)
  loadWifi();
  if (_wifi_want && _wifi_ssid[0]) { WiFi.mode(WIFI_STA); WiFi.begin(_wifi_ssid, _wifi_pass); }

  store.begin();
  loadRoomCreds();
  // Auto-login saved rooms a few seconds after boot (mesh + paths settle)
  if (_room_cred_n > 0) _auto_login_at = millis() + 8000;

  _screens[SCR_HOME]      = new HomeScreen(*this);
  _screens[SCR_CHAT]      = new ChatScreen(*this);
  _screens[SCR_CONTACTS]  = new ContactsScreen(*this);
  _screens[SCR_MAP]       = new MapScreen(*this);
  _screens[SCR_LASTHEARD] = new LastHeardScreen(*this);
  _screens[SCR_REPEATERS] = new RepeatersScreen(*this);
  _screens[SCR_TRACE]     = new TraceScreen(*this);
  _screens[SCR_NOISE]     = new NoiseScreen(*this);
  _screens[SCR_TERMINAL]  = new TerminalScreen(*this);
  _screens[SCR_SETTINGS]  = new SettingsScreen(*this);
  _screens[SCR_QR]        = new QRScreen(*this);
  _screens[SCR_ONBOARD]   = new OnboardScreen(*this);
  _screens[SCR_DIAG]      = new DiagScreen(*this);
  _screens[SCR_SOS]       = new SOSScreen(*this);
  _screens[SCR_MAPDL]     = new MapDownloadScreen(*this);
  _screens[SCR_WIFI]      = new WifiScreen(*this);
  _screens[SCR_CHANNELS]  = new ChannelsScreen(*this);
  _screens[SCR_VOICE]     = new VoiceScreen(*this);


  termLog(C_TERM_SYS, "MeshDeck v%s on MeshCore %s", MESHDECK_VERSION, FIRMWARE_VERSION);
#ifdef MESHDECK_BETA
  termLog(C_TERM_SYS, "beta build: Codec2 voice (LGPL-2.1) — see THIRD_PARTY.md");
#endif
  // NOTE: format the floats separately - StrHelper::ftoa returns a shared static
  // buffer, so calling it twice in one printf would print the same value twice.
  {
    char fq[16], bw[16];
    snprintf(fq, sizeof(fq), "%.3f", prefs->freq);
    snprintf(bw, sizeof(bw), "%.1f", prefs->bw);
    termLog(C_TERM_SYS, "node: %s  freq: %s MHz sf%d bw%s", prefs->node_name, fq, (int)prefs->sf, bw);
  }
  termLog(C_TERM_SYS, "type 'help' for commands");
  termLog(hw.hasKeyboard() ? C_TERM_SYS : C_TERM_ERR,
          "keyboard (0x55): %s", hw.hasKeyboard() ? "detected" : "NOT detected");
  termLog(hw.hasTouch() ? C_TERM_SYS : C_TERM_ERR,
          "touch (GT911): %s", hw.hasTouch() ? "detected" : "NOT detected");

  // show the home screen straight away, before any optional extras.
  // On a fresh flash / factory reset (configured==0) show the radio-preset
  // onboarding first so the user picks a mesh-compatible preset.
  _booted = true;
  _cur = set.configured ? SCR_HOME : SCR_ONBOARD;
  _screens[_cur]->enter();
  drawAll();
  _dirty = true;

  // load high-detail map packs from SD, if a card is present
  int packs = sdmaps.load(hw);
  if (packs > 0) {
    for (int i = 0; i < packs; i++) {
      const SDMapPack* p = sdmaps.pack(i);
      termLog(C_TERM_SYS, "map pack: %s (%u pts, %u places)", p->filename, p->npts, p->ncities);
    }
  } else if (packs == 0) {
    termLog(C_TERM_SYS, "sd card: no map packs in /meshdeck-maps");
  }

  // Speaker hardware self-test (after display/I2C/settings are up).
  // Hardcoded multi-tone jingle — not Codec2, not LoRa. If you hear this,
  // the amp + I2S pins work; voice silence is then encode/path, not the speaker.
  termLog(C_TERM_SYS, "audio self-test...");
  hw.playStartupSelfTest();
  termLog(C_TERM_SYS, "audio self-test done (5 tones)");
}

void UITask::reloadSDMaps() {
  int packs = sdmaps.load(hw);
  char buf[40];
  if (packs < 0) snprintf(buf, sizeof(buf), "No SD card found");
  else snprintf(buf, sizeof(buf), "%d map pack%s loaded", packs, packs == 1 ? "" : "s");
  toast(buf, packs > 0 ? C_GREEN : C_YELLOW);
  termLog(C_TERM_SYS, "%s", buf);
}

void UITask::saveSettings() {
  File f = SPIFFS.open(SETTINGS_FILE, "w");
  if (f) {
    f.write((uint8_t*)&set, sizeof(set));
    f.close();
  }
  // Also mirror to SD so the config survives a flash wipe - e.g. reflashing via
  // bmorcelli's Launcher, which erases SPIFFS. Auto-restored on next boot. (#12)
  if (hw.sdBegin()) {
    SD.mkdir("/meshdeck");
    File s = SD.open(SD_SETTINGS_FILE, FILE_WRITE);
    if (s) { s.write((uint8_t*)&set, sizeof(set)); s.close(); }
    hw.sdEnd();
  }
}

void UITask::applySettings() {
  if (set.brightness < 30) set.brightness = 200;   // never allow a black screen from a bad value
  // Sanitise the sleep timeout: only a known-good value is allowed, so a stray/
  // corrupt setting can never make the screen blank itself right after boot.
  {
    bool ok = false;
    const uint16_t valid[] = {0, 15, 30, 60, 120, 300};
    for (uint16_t v : valid) if (set.timeout_s == v) ok = true;
    if (!ok) set.timeout_s = 0;
  }
  hw.setBacklight(set.brightness);
  hw.setSound(set.sounds != 0, set.volume);
  hw.setRotationFlip(set.flip != 0);
  hw.setTouchMap(set.touch_map);
  if (set.tb_speed < 1 || set.tb_speed > 5) set.tb_speed = 3;   // default: medium
  // speed 1..5  ->  pulses-per-step 5,4,3,2,1  (higher speed = fewer pulses)
  hw.setTrackballStep(6 - set.tb_speed);
  if (set.room_login_tries < 1 || set.room_login_tries > AUTO_LOGIN_TRIES_MAX)
    set.room_login_tries = AUTO_LOGIN_TRIES_DEFAULT;
  if (set.man_lat != 0 || set.man_lon != 0) {
    if (sensors && sensors->node_lat == 0 && sensors->node_lon == 0) {
      sensors->node_lat = set.man_lat / 1000000.0;
      sensors->node_lon = set.man_lon / 1000000.0;
    }
  }
}

// ---------------------------------------------------------------- navigation

void UITask::go(ScreenId id) {
  if (id == _cur) return;
  if (_screens[_cur]) _screens[_cur]->leave();
  if (_stack_len < 8) _stack[_stack_len++] = _cur;
  _cur = id;
  _screens[_cur]->enter();
  _dirty = true;
}

void UITask::back() {
  if (_screens[_cur]) _screens[_cur]->leave();
  if (_stack_len > 0) {
    _cur = _stack[--_stack_len];
    _screens[_cur]->enter();
  } else if (_cur != SCR_HOME) {
    _cur = SCR_HOME;
    _screens[_cur]->enter();
  }
  _dirty = true;
}

void UITask::goHome() {
  if (_screens[_cur]) _screens[_cur]->leave();
  _stack_len = 0;
  _cur = SCR_HOME;
  _screens[_cur]->enter();
  _dirty = true;
}

void UITask::toast(const char* msg, uint16_t color) {
  StrHelper::strncpy(_toast, msg, sizeof(_toast));
  sanitizeAscii(_toast);  // GFX font: no UTF-8 / fancy punctuation
  _toast_color = color;
  _toast_until = millis() + 2600;
  _dirty = true;
}

void UITask::openThread(int thread_idx) {
  _pending_thread = thread_idx;
  if (_cur != SCR_CHAT) go(SCR_CHAT);
  else { _screens[SCR_CHAT]->enter(); _dirty = true; }
}

// ---- channels ----
int UITask::channelCount() {
  if (!mesh) return 0;
  int n = 0;
  ChannelDetails ch;
  while (n < MAX_GROUP_CHANNELS && mesh->getChannel(n, ch)) n++;
  return n;
}

bool UITask::channelNameAt(int idx, char* out, size_t sz) {
  ChannelDetails ch;
  if (!mesh || !mesh->getChannel(idx, ch)) return false;
  StrHelper::strncpy(out, ch.name, sz);
  return true;
}

void UITask::openChannel(int channel_idx) {
  ChannelDetails ch;
  if (!mesh || !mesh->getChannel(channel_idx, ch)) { toast("No such channel", C_RED); return; }
  DeckThread* t = store.forChannel((uint8_t)channel_idx, ch.name);
  if (t) openThread(store.indexOf(t));
}

bool UITask::addChannelNamed(const char* name, const char* psk_base64) {
  if (!mesh) return false;
  // pad base64 to a multiple of 4 with '=' (the keyboard can't type '=')
  char psk[68];
  StrHelper::strncpy(psk, psk_base64, sizeof(psk));
  int n = strlen(psk);
  while ((n % 4) != 0 && n < (int)sizeof(psk) - 1) psk[n++] = '=';
  psk[n] = 0;
  ChannelDetails* c = mesh->addChannel(name[0] ? name : "channel", psk);
  if (!c) { toast("Channel add failed (full?)", C_RED); return false; }
  mesh->saveChannels();
  toast("Channel added", C_GREEN);
  return true;
}

void UITask::openQR(const char* url) {
  StrHelper::strncpy(_qr_url, url, sizeof(_qr_url));
  go(SCR_QR);
}

// ---------------------------------------------------------------- hooks

void UITask::notify(UIEventType t) {
  _dirty = true;
}


void UITask::reresolveThreadSenders(DeckThread* t) {
  if (!t || !mesh || t->kind != TK_CONTACT) return;

  for (int i = 0; i < t->count; i++) {
    DeckMsg* m = store.msgAt(t, i);
    if (!m || !m->sender[0]) continue;

    // Already looks like a real name? skip
    // Hex fallback from our room code is exactly 8 hex chars
    bool is_hex_id = (strlen(m->sender) == 8);
    if (is_hex_id) {
      for (int k = 0; k < 8; k++) {
        char c = m->sender[k];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
          is_hex_id = false;
          break;
        }
      }
    }

    bool is_room_name = (strcmp(m->sender, t->title) == 0);

    if (!is_hex_id && !is_room_name) continue;

    // Try to resolve
    ContactInfo* author = nullptr;

    if (is_hex_id) {
      uint8_t prefix[4];
      auto hex = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
      };
      for (int b = 0; b < 4; b++) {
        prefix[b] = (hex(m->sender[b * 2]) << 4) | hex(m->sender[b * 2 + 1]);
      }

      author = mesh->lookupContactByPubKey(prefix, 4);
      if (!author) {
        int n = mesh->getNumContacts();
        for (int ci = 0; ci < n; ci++) {
          ContactInfo ct;
          if (mesh->getContactByIdx(ci, ct) &&
              memcmp(ct.id.pub_key, prefix, 4) == 0) {
            static ContactInfo found;
            found = ct;
            author = &found;
            break;
          }
        }
      }
    }

    if (author && author->name[0]) {
      StrHelper::strncpy(m->sender, author->name, sizeof(m->sender));
    }
  }

  // Persist so the fix survives reboot
  store.save();
}

void UITask::onVoiceRecv(const mesh::GroupChannel& channel,
                         const uint8_t* voice_data, size_t len,
                         bool end_of_stream, float snr) {
#ifndef MESHDECK_BETA
  (void)channel; (void)voice_data; (void)len; (void)end_of_stream; (void)snr;
  return;  // voice media is beta-only
#else
  char name[32] = "channel";
  ChannelDetails det;
  for (int i = 0; i < 40; i++) {
    if (mesh && mesh->getChannel(i, det) &&
        memcmp(&det.channel, &channel, sizeof(channel)) == 0) {
      StrHelper::strncpy(name, det.name, sizeof(name));
      break;
    }
  }

  // Live voice stays on the call UI only — do not spam chat with "voice 0.9s".
  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs) vs->pushRxVoice(voice_data, len, end_of_stream, name, snr, nullptr);

  if (!hw.isDisplayOn()) hw.displayOn();
  requestDraw();
#endif
}
void UITask::onVoicePacketSent(uint32_t tag, uint16_t len, bool eos) {
#ifndef MESHDECK_BETA
  (void)tag; (void)len; (void)eos;
#else
  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs) vs->onPacketSent(tag, len, eos);
#endif
}

void UITask::onVoicePacketAcked(uint32_t tag, bool eos) {
#ifndef MESHDECK_BETA
  (void)tag; (void)eos;
#else
  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs) vs->onPacketAcked(tag, eos);
#endif
}
void UITask::onVoiceRecvFromContact(const ContactInfo& from,
                                    const uint8_t* voice_data, size_t len,
                                    bool end_of_stream, float snr) {
#ifndef MESHDECK_BETA
  (void)from; (void)voice_data; (void)len; (void)end_of_stream; (void)snr;
  return;  // call signaling + media are beta-only
#else
  // Control frames (flags bit7) are signaling only — no beep.
  // INVITE rings once inside VoiceScreen::handleCallControl.
  // Live call media is never written to chat (was "voice 0.9s" spam each burst).
  const bool is_ctrl = (len >= 1 && (voice_data[0] & 0x80) != 0);

  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs) vs->pushRxVoice(voice_data, len, end_of_stream, from.name, snr, &from);

  if (!is_ctrl) {
    // Soft activity only for media; don't beep every packet during a call
    if (!hw.isDisplayOn()) hw.displayOn();
  }
  // Avoid redraw storms during media (chat was the old reason to draw)
  if (is_ctrl || end_of_stream)
    requestDraw();
#endif
}
void UITask::onContactMsg(const ContactInfo& from, const char* text, uint32_t sender_ts,
                          uint8_t path_len, float snr, const uint8_t* sender_prefix) {
  uint32_t ts = sender_ts ? sender_ts : epochNow();

  char sender[MD_SENDER_LEN];
  const char* body = text;
  bool got_name = false;

  // ---- 1. Resolve real author from signed-plain 4-byte prefix ----
  if (sender_prefix && mesh) {
    ContactInfo* author = mesh->lookupContactByPubKey(sender_prefix, 4);

    // Brute-force fallback in case the normal lookup misses
    if (!author) {
      int n = mesh->getNumContacts();
      for (int i = 0; i < n; i++) {
        ContactInfo ct;
        if (mesh->getContactByIdx(i, ct) &&
            memcmp(ct.id.pub_key, sender_prefix, 4) == 0) {
          static ContactInfo found;
          found = ct;
          author = &found;
          break;
        }
      }
    }

    if (author && author->name[0]) {
      StrHelper::strncpy(sender, author->name, sizeof(sender));
      got_name = true;
    } else {
      // Unknown author – show short id instead of the room name
      snprintf(sender, sizeof(sender), "%02X%02X%02X%02X",
               sender_prefix[0], sender_prefix[1],
               sender_prefix[2], sender_prefix[3]);
      got_name = true;
    }
  }

  // ---- 2. Fallback: classic "Name: message" text convention ----
  if (!got_name) {
    const char* colon = strchr(text, ':');
    if (colon && (colon - text) >= 1 && (colon - text) <= 16) {
      size_t sl = colon - text;
      bool looks_like_name = true;
      for (size_t i = 0; i < sl; i++) {
        char ch = text[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' ||
              ch == '-' || ch == '.')) {
          looks_like_name = false;
          break;
        }
      }
      int spaces = 0;
      for (size_t i = 0; i < sl; i++) if (text[i] == ' ') spaces++;
      if (spaces > 1) looks_like_name = false;

      if (looks_like_name) {
        memcpy(sender, text, sl);
        sender[sl] = 0;
        body = colon + 1;
        while (*body == ' ') body++;
        got_name = true;
      }
    }
  }

  // ---- 3. Last resort: room / contact name ----
  if (!got_name) {
    StrHelper::strncpy(sender, from.name, sizeof(sender));
  }

  if (!body || !*body) body = text;

  // ---- debug ----
  termLog(C_TERM_SYS, "onContactMsg from=%s prefix=%s -> sender='%s' text=%.40s",
          from.name,
          sender_prefix ? "yes" : "no",
          sender,
          text);

  // Sanitize for GFX (room posts can include UTF-8 / emoji)
  char sender_clean[MD_SENDER_LEN];
  char body_clean[MD_TEXT_LEN];
  StrHelper::strncpy(sender_clean, sender, sizeof(sender_clean));
  StrHelper::strncpy(body_clean, body, sizeof(body_clean));
  sanitizeAscii(sender_clean);
  sanitizeAscii(body_clean);
  if (!body_clean[0] && body && body[0]) {
    // Sanitize wiped body — keep a placeholder so the post is still visible
    StrHelper::strncpy(body_clean, "(message)", sizeof(body_clean));
  }

  // ---- store + UI ----
  DeckThread* t = store.forContact(from.id.pub_key, from.name);
  if (t) {
    store.addMsg(t, sender_clean, body_clean, ts, 0, (int8_t)(snr * 4), path_len, 0);
  } else {
    termLog(C_TERM_ERR, "store full — dropped msg from %s", from.name);
  }

  // Room/repeater: show backlog in console; establish session if missing.
  // History often arrives before (or without) a formal login RESPONSE.
  // Same receive path as upstream meshdeck; messages land in the room's chat thread.
  if (from.type == ADV_TYPE_ROOM || from.type == ADV_TYPE_REPEATER) {
    if (mesh) {
      mesh->completePendingLogin(from, true);  // no-op if not pending
      mesh->ensureServerSession(from);
    }
    if (!roomSessionOk(from.id.pub_key))
      markRoomSessionOk(from.id.pub_key);
    // Extend backlog sync quiet timer (each post = more coming)
    if (from.type == ADV_TYPE_ROOM)
      noteRoomSyncRx(from.id.pub_key);
    char line[72];
    snprintf(line, sizeof(line), "%s: %.48s", sender_clean, body_clean);
    repLog(from.name, line);
    termLog(C_TERM_RX, "[room/%s] %s: %s", from.name, sender_clean, body_clean);
  } else {
    termLog(C_TERM_RX, "[DM] %s: %s", sender_clean, body_clean);
  }

  char buf[48];
  if (from.type == ADV_TYPE_ROOM)
    snprintf(buf, sizeof(buf), "%s | %.20s", from.name, body_clean);
  else
    snprintf(buf, sizeof(buf), "%s: %.24s", sender_clean, body_clean);
  if (_cur != SCR_CHAT) toast(buf, C_GREEN);
  hw.chimeMessage();
  if (!hw.isDisplayOn()) hw.displayOn();
  hw.kickActivity();
  _dirty = true;
}

void UITask::onCliResponse(const ContactInfo& from, const char* text) {
  termLog(C_TERM_RX, "[%s] %s", from.name, text);
  repLog(from.name, text);
  _dirty = true;
}
void UITask::rememberRecentContact(const ContactInfo& c) {
  // de-dupe by 6-byte prefix
  for (int i = 0; i < _recent_ct_count; i++) {
    if (memcmp(_recent_ct[i].id.pub_key, c.id.pub_key, 6) == 0) {
      _recent_ct[i] = c;
      return;
    }
  }
  _recent_ct[_recent_ct_head] = c;
  _recent_ct_head = (_recent_ct_head + 1) % RECENT_CONTACTS;
  if (_recent_ct_count < RECENT_CONTACTS) _recent_ct_count++;
}

ContactInfo* UITask::findRecentContact(const uint8_t* prefix6) {
  for (int i = 0; i < _recent_ct_count; i++) {
    if (memcmp(_recent_ct[i].id.pub_key, prefix6, 6) == 0)
      return &_recent_ct[i];
  }
  return nullptr;
}
void UITask::onChannelMsg(uint8_t channel_idx, const char* channel_name, const char* text,
                          uint32_t ts, uint8_t path_len, float snr) {
  char sender[MD_SENDER_LEN];
  const char* body = strchr(text, ':');
  if (body && body - text < (int)sizeof(sender) + 12) {
    size_t sl = body - text;
    if (sl >= sizeof(sender)) sl = sizeof(sender) - 1;
    memcpy(sender, text, sl);
    sender[sl] = 0;
    body++;
    while (*body == ' ') body++;
  } else {
    strcpy(sender, "?");
    body = text;
  }

  // Prefer a plausible sender timestamp; otherwise use local epoch
  uint32_t now = epochNow();
  uint32_t msg_ts = ts;
  if (msg_ts < 1000000000UL || msg_ts > now + 3600UL) {
    msg_ts = now;   // not a sane unix epoch → use receive time
  }

  termLog(C_TERM_SYS,
          "chMsg idx=%u ts_in=%lu now=%lu use=%lu hops=%u snr=%.1f from=%s",
          (unsigned)channel_idx,
          (unsigned long)ts,
          (unsigned long)now,
          (unsigned long)msg_ts,
          (unsigned)path_len,
          snr,
          sender);

  DeckThread* t = store.forChannel(channel_idx, channel_name);
  if (t) {
    store.addMsg(t, sender, body, msg_ts, 0, (int8_t)(snr * 4), path_len, 0);
  }

  termLog(C_TERM_RX, "[#%s] %s", channel_name, text);
  char buf[48];
  snprintf(buf, sizeof(buf), "#%s %.20s", channel_name, text);
  if (_cur != SCR_CHAT) toast(buf, C_ACCENT);
  hw.chimeMessage();
  if (!hw.isDisplayOn()) hw.displayOn();
  hw.kickActivity();
  _dirty = true;
}

void UITask::onAckDelivered(uint32_t ack, const ContactInfo* contact, uint32_t trip_ms) {
  if (store.markDelivered(ack)) {
    termLog(C_TERM_SYS, "delivered (%u ms)", trip_ms);
    _dirty = true;
  }
}

void UITask::onAdvertSeen(const ContactInfo& contact, bool is_new, uint8_t path_len) {
  rememberRecentContact(contact);
  
  HeardEntry* e = &_heard[_heard_head];
  memset(e, 0, sizeof(*e));
  StrHelper::strncpy(e->name, contact.name, sizeof(e->name));
  e->type = contact.type;
  e->hops = path_len;
  e->snr4 = mesh ? (int8_t)(mesh->getLastSNR() * 4) : 0;
  e->rssi = mesh ? (int16_t)mesh->getLastRSSI() : 0;
  e->at = epochNow();
  e->lat = contact.gps_lat;
  e->lon = contact.gps_lon;
  memcpy(e->prefix, contact.id.pub_key, 6);
  _heard_head = (_heard_head + 1) % HEARD_MAX;
  if (_heard_count < HEARD_MAX) _heard_count++;

  termLog(C_TERM_SYS, "advert: %s (%s%s)", contact.name,
          contact.type == ADV_TYPE_REPEATER ? "repeater" :
          contact.type == ADV_TYPE_ROOM ? "room" : "chat",
          is_new ? ", new" : "");
  if (is_new) {
    char buf[48];
    snprintf(buf, sizeof(buf), "New node: %s", contact.name);
    toast(buf, C_PURPLE);
  }
  _dirty = true;

#ifdef MESHDECK_BETA
  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs) vs->onTargetAdvert(contact);
#endif

}

void UITask::onRawRx(float snr, float rssi, int len) {
  _last_rx_rssi = rssi;
  _last_rx_snr = snr;
  _last_rx_millis = millis();
  _rx_count++;
  if (_cur == SCR_DIAG) _dirty = true;
  termLog(C_TERM_SYS, "rx: %d bytes, rssi %d, snr %s", len, (int)rssi, StrHelper::ftoa(snr));
}

void UITask::onTraceResult(uint32_t tag, uint8_t path_len, const uint8_t* path_hashes,
                           const uint8_t* path_snrs, uint8_t snr_count, float final_snr) {
  if (trace.tag != 0 && tag == trace.tag) {
    trace.valid = true;
    trace.hops = snr_count > 16 ? 16 : snr_count;
    for (int i = 0; i < trace.hops; i++) {
      trace.snrs[i] = (int8_t)path_snrs[i];
      trace.hashes[i] = i < path_len ? path_hashes[i] : 0;
    }
    trace.final_snr = final_snr;
    trace.at_millis = millis();
    termLog(C_TERM_SYS, "trace back: %d hops, final snr %s", trace.hops, StrHelper::ftoa(final_snr));
    toast("Trace complete", C_GREEN);
    _dirty = true;
  }
}

void UITask::onSendTimeout() {
  store.markTimedOut();
  termLog(C_TERM_ERR, "send timeout (no ack)");
  _dirty = true;
}

void UITask::logF(const char* fmt, ...) {
  char buf[TERM_COLS];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  termLog(C_TERM_SYS, "%s", buf);
}

// ---------------------------------------------------------------- terminal

void UITask::termLog(uint16_t color, const char* fmt, ...) {
  if (!_term) return;
  TermLine* l = &_term[_term_head];
  l->color = color;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(l->text, TERM_COLS, fmt, ap);
  va_end(ap);
  _term_head = (_term_head + 1) % TERM_LINES;
  if (_term_count < TERM_LINES) _term_count++;
  Serial.println(l->text);   // mirror to USB serial
  if (_cur == SCR_TERMINAL) _dirty = true;
}

TermLine* UITask::termLine(int i) {
  if (!_term || i < 0 || i >= _term_count) return nullptr;
  int start = (_term_head - _term_count + TERM_LINES * 2) % TERM_LINES;
  return &_term[(start + i) % TERM_LINES];
}

const HeardEntry* UITask::heardAt(int i) const {
  if (i < 0 || i >= _heard_count) return nullptr;
  int idx = (_heard_head - 1 - i + HEARD_MAX * 2) % HEARD_MAX;
  return &_heard[idx];
}

// ---------------------------------------------------------------- actions

ContactInfo* UITask::contactByPrefix(const uint8_t* prefix6) {
  return mesh ? mesh->lookupContactByPubKey(prefix6, 6) : nullptr;
}

bool UITask::sendDM(const uint8_t* pub_prefix, const char* text) {
  ContactInfo* c = contactByPrefix(pub_prefix);
  if (!c) { toast("Contact not found", C_RED); return false; }
  if (!allowSendToContact(*c, true)) return false;
  uint32_t expected_ack = 0, est_timeout = 0;
  int res = mesh->sendMessage(*c, epochNow(), 0, text, expected_ack, est_timeout);
  if (res == MSG_SEND_FAILED) {
    toast("Send failed", C_RED);
    hw.chimeError();
    return false;
  }
  mesh->registerExpectedAck(expected_ack, c);
  DeckThread* t = store.forContact(c->id.pub_key, c->name);
  if (t) store.addMsg(t, prefs->node_name, text, epochNow(), MF_OUT,
                      0, res == MSG_SEND_SENT_FLOOD ? 0xFF : c->out_path_len, expected_ack);
  termLog(C_TERM_TX, "[DM->%s] %s", c->name, text);
  return true;
}

bool UITask::sendChannel(uint8_t channel_idx, const char* text) {
  ChannelDetails ch;
  if (!mesh->getChannel(channel_idx, ch)) { toast("No such channel", C_RED); return false; }
  if (!mesh->sendGroupMessage(epochNow(), ch.channel, prefs->node_name, text, strlen(text))) {
    toast("Send failed", C_RED);
    hw.chimeError();
    return false;
  }
  DeckThread* t = store.forChannel(channel_idx, ch.name);
  if (t) store.addMsg(t, prefs->node_name, text, epochNow(), MF_OUT | MF_DELIVERED, 0, 0, 0);
  termLog(C_TERM_TX, "[#%s] %s: %s", ch.name, prefs->node_name, text);
  return true;
}

bool UITask::startTrace(const ContactInfo& target) {
  if (target.out_path_len == 0xFF) {
    toast("No known path yet", C_YELLOW);
    return false;
  }
  uint32_t tag = mesh->sendTracePath(target.out_path, target.out_path_len);
  if (!tag) { toast("Trace failed to send", C_RED); return false; }
  memset(&trace, 0, sizeof(trace));
  trace.tag = tag;
  trace.sent_millis = millis();
  StrHelper::strncpy(trace.target, target.name, sizeof(trace.target));
  termLog(C_TERM_TX, "trace -> %s (%d hops out)", target.name, target.out_path_len);
  return true;
}

bool UITask::startVoiceCall(const ContactInfo& to) {
#ifdef MESHDECK_BETA
  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (!vs) {
    toast("Voice unavailable", C_RED);
    return false;
  }
  if (vs->isInCall()) {
    toast("Already in a call", C_YELLOW);
    return false;
  }
  if (!mesh) {
    toast("No mesh", C_RED);
    return false;
  }
  // Voice is local-only: direct or 1 hop (e.g. your own repeater). No mesh flood.
  ContactInfo live = to;
  if (ContactInfo* p = mesh->lookupContactByPubKey(to.id.pub_key, 6))
    live = *p;
  if (!mesh->canVoiceCallContact(live)) {
    uint8_t hops = MyMesh::voiceHopCount(live.out_path_len);
    char msg[56];
    snprintf(msg, sizeof(msg), "Call needs <=%u hop (path %u hops)",
             (unsigned)MyMesh::VOICE_MAX_HOPS, (unsigned)hops);
    toast(msg, C_YELLOW);
    termLog(C_TERM_ERR, "voice call blocked: %s path hops=%u max=%u",
            live.name, (unsigned)hops, (unsigned)MyMesh::VOICE_MAX_HOPS);
    return false;
  }
  {
    uint8_t hops = MyMesh::voiceHopCount(live.out_path_len);
    if (hops == 0xFF)
      termLog(C_TERM_SYS, "voice call %s: no path → zero-hop direct", live.name);
    else
      termLog(C_TERM_SYS, "voice call %s: %u hop path", live.name, (unsigned)hops);
  }
  vs->prepareOutbound(live);
  go(SCR_VOICE);   // enter() auto-sends INVITE
  return true;
#else
  (void)to;
  toast("Voice requires beta build", C_YELLOW);
  return false;
#endif
}

void UITask::repLog(const char* from, const char* text) {
  // Sanitize for GFX font before queueing on the console list
  char fbuf[32], tbuf[72];
  StrHelper::strncpy(fbuf, from ? from : "?", sizeof(fbuf));
  StrHelper::strncpy(tbuf, text ? text : "", sizeof(tbuf));
  sanitizeAscii(fbuf);
  sanitizeAscii(tbuf);
  RepeatersScreen* r = (RepeatersScreen*)_screens[SCR_REPEATERS];
  if (r) r->onCliResponse(fbuf, tbuf);
}

// ---------------------------------------------------------------- status helpers

int UITask::batteryPercent() const {
  int mv = getBattMilliVolts();
  if (mv <= 0) return -1;
  int pct = (mv - 3350) * 100 / (4200 - 3350);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

int UITask::meshBars() const {
  uint32_t now = epochNow();
  int n = 0;
  float best_snr = -30;
  for (int i = 0; i < _heard_count; i++) {
    const HeardEntry* e = heardAt(i);
    if (e && now - e->at < 1800) {
      n++;
      if (e->snr4 / 4.0f > best_snr) best_snr = e->snr4 / 4.0f;
    }
  }
  if (n == 0) return millis() - _last_rx_millis < 120000 ? 1 : 0;
  if (best_snr > 5) return 4;
  if (best_snr > 0) return 3;
  if (best_snr > -10) return 2;
  return 1;
}

bool UITask::ownPos(double& lat, double& lon) const {
  if (sensors && (sensors->node_lat != 0 || sensors->node_lon != 0)) {
    lat = sensors->node_lat;
    lon = sensors->node_lon;
    return true;
  }
  if (set.man_lat != 0 || set.man_lon != 0) {
    lat = set.man_lat / 1000000.0;
    lon = set.man_lon / 1000000.0;
    return true;
  }
  lat = lon = 0;
  return false;
}

uint32_t UITask::epochNow() const {
  return mesh ? mesh->getRTCClock()->getCurrentTime() : 0;
}

uint32_t UITask::localEpoch() const {
  uint32_t e = epochNow();
  if (e < 1000000000) return e;                       // clock not set yet
  return e + (int32_t)set.tz_offset * 3600;           // apply timezone offset
}

void UITask::fmtClock(char* out, size_t sz) const {
  uint32_t e = localEpoch();
  if (e < 1000000000) { snprintf(out, sz, "--:--"); return; }
  DateTime dt(e);
  snprintf(out, sz, "%02d:%02d", dt.hour(), dt.minute());
}

void UITask::fmtAgo(char* out, size_t sz, uint32_t then) const {
  uint32_t now = epochNow();
  if (then == 0 || then > now) {
    snprintf(out, sz, "-");
    return;
  }
  uint32_t d = now - then;
  if (d < 60) snprintf(out, sz, "%lus", (unsigned long)d);
  else if (d < 3600) snprintf(out, sz, "%lum", (unsigned long)(d / 60));
  else if (d < 86400) snprintf(out, sz, "%luh", (unsigned long)(d / 3600));
  else snprintf(out, sz, "%lud", (unsigned long)(d / 86400));
}

void UITask::fmtContactPath(char* out, size_t sz, const ContactInfo& ct) const {
  if (!out || sz == 0) return;
  out[0] = 0;
  // 0xFF / OUT_PATH_UNKNOWN = no stored direct path (send will flood)
  if (ct.out_path_len == 0xFF) {
    snprintf(out, sz, "flood (no path)");
    return;
  }
  // path_len packs hop count (low 6 bits) and hash size (high 2 bits: 0→1B, 1→2B, 2→3B)
  uint8_t hops = (uint8_t)(ct.out_path_len & 63);
  uint8_t hsz  = (uint8_t)((ct.out_path_len >> 6) + 1);
  if (hsz > 3) hsz = 1;
  if (hops == 0) {
    snprintf(out, sz, "direct");
    return;
  }
  size_t used = 0;
  for (uint8_t h = 0; h < hops; h++) {
    if (h > 0) {
      if (used + 4 >= sz) break;
      out[used++] = ' ';
      out[used++] = '>';
      out[used++] = ' ';
      out[used] = 0;
    }
    for (uint8_t b = 0; b < hsz; b++) {
      if (used + 3 >= sz) break;
      uint8_t v = ct.out_path[(size_t)h * hsz + b];
      static const char* hex = "0123456789abcdef";
      out[used++] = hex[(v >> 4) & 0xF];
      out[used++] = hex[v & 0xF];
      out[used] = 0;
    }
  }
  if (used == 0) snprintf(out, sz, "direct");
}

void UITask::drawStatusBar(const char* title) {
  GFXcanvas16& c = cv();
  c.fillRect(0, 0, SCREEN_W, STATUS_H, C_BG_ALT);
  c.drawFastHLine(0, STATUS_H, SCREEN_W, C_FG_FAINT);
  c.setTextSize(1);
  c.setTextColor(C_FG);
  c.setCursor(6, 5);
  char t[36];
  ellipsize(t, 30, title);
  c.print(t);

  // clock (center)
  char clk[8];
  fmtClock(clk, sizeof(clk));
  c.setCursor(SCREEN_W / 2 - 15, 5);
  c.setTextColor(C_FG_DIM);
  c.print(clk);

  int x = SCREEN_W - 6;

  // battery
  int pct = batteryPercent();
  x -= 24;
  c.drawRect(x, 5, 18, 9, C_FG_DIM);
  c.fillRect(x + 18, 7, 2, 5, C_FG_DIM);
  if (pct >= 0) {
    uint16_t bc = pct > 30 ? C_GREEN : pct > 15 ? C_YELLOW : C_RED;
    c.fillRect(x + 2, 7, (14 * pct) / 100, 5, bc);
  }

  // mesh signal bars
  int bars = meshBars();
  x -= 26;
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 3;
    uint16_t bc = i < bars ? C_ACCENT : C_FG_FAINT;
    c.fillRect(x + i * 5, 14 - bh, 3, bh, bc);
  }

  // unread badge
  int unread = ((UITask*)this)->store.totalUnread();
  if (unread > 0) {
    x -= 24;
    c.fillRoundRect(x, 3, 20, 12, 6, C_RED);
    c.setTextColor(0xFFFF);
    c.setCursor(x + (unread > 9 ? 4 : 8), 5);
    c.print(unread > 99 ? 99 : unread);
  }

  // GPS fix marker (green = has a position fix, faint = on but no fix yet)
  if (prefs && prefs->gps_enabled) {
    x -= 12;
    c.setTextColor(gpsFix() ? C_GREEN : C_FG_FAINT);
    c.setCursor(x, 5);
    c.print("G");
  }
  // WiFi marker (green = connected, yellow = connecting)
  int ws = wifiState();
  if (ws) {
    x -= 12;
    c.setTextColor(ws == 2 ? C_GREEN : C_YELLOW);
    c.setCursor(x, 5);
    c.print("W");
  }

  // BLE connected marker
  if (hasConnection()) {
    x -= 12;
    c.setTextColor(C_ACCENT);
    c.setCursor(x, 5);
    c.print("B");
  }
}

// ---------------------------------------------------------------- main loop

void UITask::loop() {
  if (!_booted) return;

  store.loop();
  if (_web) _web->handleClient();          // remote screen web server

  // noise floor sampling (4x/sec)
  if (millis() - _last_noise_sample > 250) {
    _last_noise_sample = millis();
    _last_noise = mesh->getNoiseFloorNow();
    int8_t v = _last_noise < -128 ? -128 : (_last_noise > 0 ? 0 : (int8_t)_last_noise);
    _noise[_noise_head] = v;
    _noise_head = (_noise_head + 1) % NOISE_SAMPLES;
    if (_cur == SCR_NOISE && hw.isDisplayOn()) _dirty = true;
  }

  dispatchInput();

#ifdef MESHDECK_BETA
  // TX: drain encode queue on loop. RX decode runs on dedicated c2dec task
  // (large internal stack; half-duplex with c2work).
  {
    VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
    if (vs && (_cur == SCR_VOICE || vs->isInCall())) {
      vs->pollPTT();
      vs->pollRxPlayback();  // ensures c2dec is alive while listening
    }
  }
#endif

  // auto-advert: periodic flood advert so nearby nodes keep discovering us
  if (set.adv_interval_min > 0) {
    uint32_t period = (uint32_t)set.adv_interval_min * 60000UL;
    if (millis() - _last_auto_adv > period) {
      _last_auto_adv = millis();
      if (mesh) { mesh->advertFlood(); termLog(C_TERM_TX, "auto-advert (flood)"); }
    }
  }

  // SOS beacon: repeat an SOS + latest position until cancelled
  if (_sos_active && millis() - _sos_last > 120000UL) {
    sendSOSNow();
  }

  // NTP clock sync: when WiFi is connected, fetch UTC once and set the RTC.
  if (wifiState() == 2) {
    if (!_ntp_started) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      _ntp_started = true;
      _ntp_last_try = millis();
    } else if (!_ntp_done && millis() - _ntp_last_try > 500) {
      _ntp_last_try = millis();
      time_t now = time(nullptr);
      if (now > 1700000000) {
        if (mesh) mesh->getRTCClock()->setCurrentTime((uint32_t)now);
        _ntp_done = true;
        toast("Clock synced (NTP)", C_GREEN);
      }
    }
  } else {
    _ntp_started = false;
    _ntp_done = false;
    if (_remote_on) stopRemoteScreen();
  }

  // USB serial -> terminal commands
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (_ser_len > 0) {
        _ser_line[_ser_len] = 0;
        _ser_len = 0;
        TerminalScreen* t = (TerminalScreen*)_screens[SCR_TERMINAL];
        t->execCommand(_ser_line);
      }
    } else if (_ser_len < (int)sizeof(_ser_line) - 1) {
      _ser_line[_ser_len++] = ch;
    }
  }

  // 1 Hz tick for clocks etc
  if (millis() - _last_tick > 1000) {
    _last_tick = millis();
    // Stuck login pending: free UI so ENTER on password can work again
    if (_login_pending_valid && _login_pending_ms &&
        (int32_t)(millis() - _login_pending_ms) > (int32_t)LOGIN_PENDING_TIMEOUT_MS) {
      clearLoginPending("timeout (no response)");
      toast("Login timed out - try again", C_YELLOW);
      RepeatersScreen* rs = (RepeatersScreen*)_screens[SCR_REPEATERS];
      if (rs) rs->onLoginFinished("sys", false);
      // Retry auto-login chain after a short pause
      _auto_login_at = millis() + 1500;
    }
    // End room sync windows that finished quietly / hit max; refresh countdown UI
    for (int i = 0; i < ROOM_SYNC_SLOTS; i++) {
      if (!_room_sync[i].active) continue;
      if (roomSyncRemainingMs(_room_sync[i].prefix) == 0)
        endRoomSync(_room_sync[i].prefix, "quiet/timeout");
      else
        _dirty = true;  // countdown chip / compose hint
    }
    if (_auto_login_at && (int32_t)(millis() - _auto_login_at) >= 0) {
      _auto_login_at = 0;
      tryAutoLoginRooms();
    }
    _screens[_cur]->tick1s();
#ifdef MESHDECK_BETA
    // Ring timeout / Codec2 finish while user is on another screen (incoming)
    if (_cur != SCR_VOICE) {
      VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
      if (vs && (vs->hasIncomingCall() || vs->isInCall()))
        vs->tick1s();
    }
#endif
    if ((_cur == SCR_HOME || _cur == SCR_DIAG) && hw.isDisplayOn()) _dirty = true;
#ifdef MESHDECK_BETA
    {
      VoiceScreen* vs_in = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
      if (vs_in && vs_in->hasIncomingCall()) {
        _dirty = true;
        hw.kickActivity();  // don't sleep while ringing
      }
    }
#endif
    checkDim();
  }

  // keep the Settings screen live so the input-test readout updates smoothly
  if (_cur == SCR_SETTINGS && hw.isDisplayOn() && millis() - _last_settings_refresh > 100) {
    _last_settings_refresh = millis();
    _dirty = true;
  }

  if (_toast_until && millis() > _toast_until) {
    _toast_until = 0;
    _dirty = true;
  }

  if (_dirty && hw.isDisplayOn()) {
    _dirty = false;
    drawAll();
  }
}
void UITask::checkDim() {
  if (set.timeout_s == 0) return;
  uint32_t idle = millis() - hw.lastActivityMillis();
  bool expired = idle > (uint32_t)set.timeout_s * 1000;

  if (set.always_on) {
    // Always-On Clock: don't power the panel off (the clock must stay visible),
    // but after the timeout drop to a low backlight to save battery instead of
    // sitting at full brightness. Restore the user's brightness on activity. (#1)
    const uint8_t DIM = 30;               // matches the min brightness (30..255)
    if (hw.isDisplayOn()) {
      uint8_t target = expired ? DIM : set.brightness;
      if (target != _dim_level) { hw.setBacklight(target); _dim_level = target; }
    }
    return;
  }

  if (hw.isDisplayOn() && expired) hw.displayOff();
}

// Remap a letter key to the number/symbol printed on it (T-Deck legend), used
// when the software symbol layer is active. Numbers/other chars pass through.
static uint8_t symMap(uint8_t k) {
  uint8_t c = (k >= 'A' && k <= 'Z') ? (uint8_t)(k - 'A' + 'a') : k;
  switch (c) {
    case 'q': return '#'; case 'w': return '1'; case 'e': return '2'; case 'r': return '3';
    case 't': return '('; case 'y': return ')'; case 'u': return '_'; case 'i': return '-';
    case 'o': return '+'; case 'p': return '@';
    case 'a': return '*'; case 's': return '4'; case 'd': return '5'; case 'f': return '6';
    case 'g': return '/'; case 'h': return ':'; case 'j': return ';'; case 'k': return '\'';
    case 'l': return '"';
    case 'z': return '7'; case 'x': return '8'; case 'c': return '9'; case 'v': return '?';
    case 'b': return '!'; case 'n': return ','; case 'm': return '.';
    case '$': return '0';
    default:  return k;   // digits, space, enter, backspace, etc. unchanged
  }
}

void UITask::dispatchInput() {
  // any input wakes the display
  uint8_t k = hw.readKey();
  if (k) Serial.printf("[kbd] raw=%u\n", k);   // diagnostics: what the keyboard sends

  // Software number/symbol layer. The stock keyboard chip's Alt key does NOT
  // emit numbers on many T-Deck units, so we provide our own. Alt+C reaches us
  // as 0x0C and toggles the layer; when active, letters map to their printed
  // number/symbol. (Settings text fields also toggle it with the trackball.)
  if (k == 0x0C) {
    _sym_shift = !_sym_shift;
    toast(_sym_shift ? "123 / symbols ON" : "letters", _sym_shift ? C_ACCENT : C_FG_DIM);
    _dirty = true;
    k = 0;
  }
  if (k && _sym_shift) k = symMap(k);
  if (!k && _inj_key) { k = _inj_key; _inj_key = 0; }        // web remote-screen key
  NavEvent nv = hw.readNav();
  if (nv == NAV_NONE && _inj_nav) { nv = (NavEvent)_inj_nav; _inj_nav = 0; }
  TouchEvent te;
  bool has_touch = hw.readTouch(te);
  if (!has_touch && _inj_tap) {
    te.kind = TouchEvent::TAP; te.x = _inj_x; te.y = _inj_y; has_touch = true; _inj_tap = false;
  }

  if (!hw.isDisplayOn()) {
    if (k || nv != NAV_NONE || has_touch) {
      hw.displayOn();
      _dirty = true;
    }
    return;
  }

  Screen* s = _screens[_cur];
  bool used = false;

#ifdef MESHDECK_BETA
  // Incoming voice call: Accept/Decline captures all input above the screen
  VoiceScreen* vs_ring = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs_ring && vs_ring->hasIncomingCall()) {
    if (k) {
      if (k == 0x0D || k == ' ') {
        vs_ring->acceptInbound();
        go(SCR_VOICE);
      } else if (k == 'n' || k == 'N' || k == 0x1B) {
        vs_ring->rejectInbound(true);
      }
      // swallow everything else while ringing
      used = true;
      _dirty = true;
    }
    if (nv != NAV_NONE) {
      if (nv == NAV_SELECT) {
        vs_ring->acceptInbound();
        go(SCR_VOICE);
      } else if (nv == NAV_BACK) {
        vs_ring->rejectInbound(true);
      }
      used = true;
      _dirty = true;
    }
    if (has_touch) {
      // Tap left half = decline, right half = accept
      if (te.kind == TouchEvent::TAP) {
        if (te.x < SCREEN_W / 2)
          vs_ring->rejectInbound(true);
        else {
          vs_ring->acceptInbound();
          go(SCR_VOICE);
        }
      }
      used = true;
      _dirty = true;
    }
    return;
  }
#endif

  if (k) {
    used = s->key(k);
    if (!used) {
      // keyboard fallback navigation: only fires when the current screen did
      // NOT consume the key, so text screens (chat/terminal) are unaffected.
      // i/k = up/down, j/l = left/right, space/enter = select.
      switch (k) {
        case 'i': case 'I': used = s->nav(NAV_UP); break;
        case 'k': case 'K': used = s->nav(NAV_DOWN); break;
        case 'j': case 'J': used = s->nav(NAV_LEFT); break;
        case 'l': case 'L': used = s->nav(NAV_RIGHT); break;
        case ' ': used = s->nav(NAV_SELECT); break;
        case 0x08:            // backspace
        case 0x7F:            // delete (some T-Deck keebs send this for backspace)
        case 0x1B:            // esc
        case '`':             // handy always-back key
          back(); used = true; break;
      }
    }
    _dirty = true;
  }
  if (nv != NAV_NONE) {
    used = s->nav(nv);
    if (!used && nv == NAV_BACK) back();   // trackball press-and-hold = back
    _dirty = true;
  }
  if (has_touch) {
    used = s->touch(te);
    if (!used && te.kind == TouchEvent::RELEASE && te.dx > 90 && abs(te.dy) < 60) back();  // swipe right = back
    _dirty = true;
  }
}

void UITask::drawAll() {
  _screens[_cur]->draw();

  // toast popup overlays everything
  if (_toast_until && millis() < _toast_until && _toast[0]) {
    GFXcanvas16& c = cv();
    int w = strlen(_toast) * 6 + 20;
    if (w > SCREEN_W - 10) w = SCREEN_W - 10;
    int x = (SCREEN_W - w) / 2;
    c.fillRoundRect(x, 24, w, 22, 8, C_BG_RAISED);
    c.drawRoundRect(x, 24, w, 22, 8, _toast_color);
    c.setTextSize(1);
    c.setTextColor(C_FG);
    c.setCursor(x + 10, 31);
    char t[52];
    ellipsize(t, (w - 16) / 6 + 1 > 51 ? 51 : (w - 16) / 6 + 1, _toast);
    c.print(t);
  }

#ifdef MESHDECK_BETA
  // Incoming call modal sits above the current screen (and toast)
  VoiceScreen* vs = static_cast<VoiceScreen*>(_screens[SCR_VOICE]);
  if (vs && vs->hasIncomingCall() && _cur != SCR_VOICE) {
    vs->drawIncomingOverlay(cv());
  }
#endif

  hw.push();
}

// ---------------------------------------------------------------- discovery / SOS / presets

void UITask::discover() {
  if (mesh) mesh->advertFlood();
  toast("Advert sent - listening...", C_CYAN);
  termLog(C_TERM_TX, "discover: flood advert sent");
  go(SCR_LASTHEARD);
}

void UITask::applyPreset(float freq, float bw, uint8_t sf, uint8_t cr) {
  if (!prefs || !mesh) return;
  prefs->freq = freq;
  prefs->bw = bw;
  prefs->sf = sf;
  prefs->cr = cr;
  mesh->applyRadioPrefs();
  mesh->savePrefs();
  char fq[16];
  snprintf(fq, sizeof(fq), "%.3f", freq);
  termLog(C_TERM_SYS, "radio preset: %s MHz sf%d bw%.1f cr%d", fq, (int)sf, bw, (int)cr);
}

void UITask::finishOnboarding() {
  set.configured = 1;
  saveSettings();
  goHome();
}

void UITask::toggleSOS() {
  if (set.sos_disabled) { toast("SOS is disabled in Settings", C_YELLOW); return; }
  if (!_sos_active) {
    // Require a real location before the beacon can arm: a live GPS fix if a
    // module is connected, otherwise the manual position from Settings.
    double lat, lon;
    if (!ownPos(lat, lon)) {
      toast("Set a location first (GPS or manual)", C_YELLOW);
      hw.chimeError();
      _dirty = true;
      return;
    }
    _sos_active = true;
    hw.chimeError();
    _sos_last = 0;          // force an immediate first broadcast
    sendSOSNow();
    toast("SOS ACTIVE - broadcasting", C_RED);
  } else {
    _sos_active = false;
    toast("SOS cancelled", C_YELLOW);
    termLog(C_TERM_SYS, "SOS cancelled");
  }
  _dirty = true;
}

void UITask::sendSOSNow() {
  _sos_last = millis();
  char msg[110];
  double lat, lon;
  if (ownPos(lat, lon)) {
    char slat[16], slon[16];
    snprintf(slat, sizeof(slat), "%.5f", lat);
    snprintf(slon, sizeof(slon), "%.5f", lon);
    snprintf(msg, sizeof(msg), "SOS! %s needs help @ %s,%s",
             prefs ? prefs->node_name : "?", slat, slon);
  } else {
    snprintf(msg, sizeof(msg), "SOS! %s needs help (no GPS fix)",
             prefs ? prefs->node_name : "?");
  }
  sendChannel(0, msg);   // public channel
  termLog(C_TERM_TX, "%s", msg);
}

// ---------------------------------------------------------------- Room / repeater credentials

void UITask::loadRoomCreds() {
  _room_cred_n = 0;
  memset(_room_session_ok, 0, sizeof(_room_session_ok));
  memset(_room_auto_tries, 0, sizeof(_room_auto_tries));
  _auto_login_wait_rounds = 0;
  File f = SPIFFS.open(ROOM_CRED_FILE, "r");
  if (!f) return;
  uint32_t magic = 0;
  int n = 0;
  if (f.read((uint8_t*)&magic, 4) != 4 || magic != ROOM_CRED_MAGIC) { f.close(); return; }
  if (f.read((uint8_t*)&n, 4) != 4 || n < 0 || n > ROOM_CRED_MAX) { f.close(); return; }
  for (int i = 0; i < n; i++) {
    if (f.read((uint8_t*)&_room_creds[i], sizeof(RoomCred)) != sizeof(RoomCred)) break;
    _room_creds[i].password[sizeof(_room_creds[i].password) - 1] = 0;
    _room_creds[i].name[sizeof(_room_creds[i].name) - 1] = 0;
    _room_cred_n++;
  }
  f.close();
  termLog(C_TERM_SYS, "room logins: %d saved", _room_cred_n);
}

int UITask::roomCredIndex(const uint8_t* prefix6) const {
  if (!prefix6) return -1;
  for (int i = 0; i < _room_cred_n; i++)
    if (memcmp(_room_creds[i].pub_prefix, prefix6, 6) == 0) return i;
  return -1;
}

void UITask::markRoomSessionOk(const uint8_t* prefix6) {
  int i = roomCredIndex(prefix6);
  if (i >= 0) _room_session_ok[i] = 1;
}

bool UITask::roomSessionOk(const uint8_t* prefix6) const {
  int i = roomCredIndex(prefix6);
  return i >= 0 && _room_session_ok[i] != 0;
}

uint8_t UITask::autoLoginMaxTries() const {
  uint8_t n = set.room_login_tries;
  if (n < 1) n = AUTO_LOGIN_TRIES_DEFAULT;
  if (n > AUTO_LOGIN_TRIES_MAX) n = AUTO_LOGIN_TRIES_MAX;
  return n;
}

bool UITask::roomNeedsAutoLogin(int idx) const {
  if (idx < 0 || idx >= _room_cred_n) return false;
  const RoomCred& e = _room_creds[idx];
  // auto_login alone is enough — blank passwords are valid for some rooms
  if (!e.auto_login) return false;
  if (_room_session_ok[idx]) return false;
  if (_room_auto_tries[idx] >= autoLoginMaxTries()) return false;
  if (!mesh) return false;
  ContactInfo* live = mesh->lookupContactByPubKey(e.pub_prefix, 6);
  // Contact not in book yet — still "needs" auto-login when it appears
  if (!live) return true;
  if (mesh->isLoggedInto(live->id.pub_key)) return false;
  return true;
}

void UITask::clearLoginPending(const char* why) {
  if (!_login_pending_valid) return;
  _login_pending_valid = false;
  _login_pending_ms = 0;
  _login_pending_pwd[0] = 0;
  if (why) termLog(C_TERM_SYS, "login pending cleared: %s", why);
}

int UITask::roomSyncIndex(const uint8_t* prefix6) const {
  if (!prefix6) return -1;
  for (int i = 0; i < ROOM_SYNC_SLOTS; i++) {
    if (_room_sync[i].active &&
        memcmp(_room_sync[i].prefix, prefix6, 6) == 0)
      return i;
  }
  return -1;
}

void UITask::beginRoomSync(const uint8_t* prefix6) {
  if (!prefix6) return;
  int idx = roomSyncIndex(prefix6);
  if (idx < 0) {
    // Free slot or replace oldest
    idx = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < ROOM_SYNC_SLOTS; i++) {
      if (!_room_sync[i].active) { idx = i; break; }
      if (_room_sync[i].login_ms < oldest) {
        oldest = _room_sync[i].login_ms;
        idx = i;
      }
    }
  }
  memcpy(_room_sync[idx].prefix, prefix6, 6);
  _room_sync[idx].login_ms = millis();
  _room_sync[idx].last_rx_ms = millis();  // require quiet after login too
  _room_sync[idx].active = true;
  termLog(C_TERM_SYS, "room sync started (backlog) — wait before posting");
  toast("Syncing room backlog...", C_CYAN);
  _dirty = true;
}

void UITask::noteRoomSyncRx(const uint8_t* prefix6) {
  int idx = roomSyncIndex(prefix6);
  if (idx < 0) return;  // not in a sync window for this room
  _room_sync[idx].last_rx_ms = millis();
  _dirty = true;
}

void UITask::endRoomSync(const uint8_t* prefix6, const char* why) {
  int idx = roomSyncIndex(prefix6);
  if (idx < 0) return;
  _room_sync[idx].active = false;
  termLog(C_TERM_SYS, "room sync done%s%s",
          why ? ": " : "", why ? why : "");
  toast("Room ready — you can post", C_GREEN);
  _dirty = true;
}

uint32_t UITask::roomSyncRemainingMs(const uint8_t* prefix6) const {
  int idx = roomSyncIndex(prefix6);
  if (idx < 0) return 0;
  const RoomSync& s = _room_sync[idx];
  if (!s.active) return 0;
  uint32_t now = millis();
  uint32_t since_login = now - s.login_ms;
  uint32_t since_rx    = now - s.last_rx_ms;

  // Hard cap always wins
  if (since_login >= ROOM_SYNC_MAX_MS) return 0;

  uint32_t need = 0;
  if (since_login < ROOM_SYNC_MIN_MS)
    need = ROOM_SYNC_MIN_MS - since_login;
  if (since_rx < ROOM_SYNC_QUIET_MS) {
    uint32_t q = ROOM_SYNC_QUIET_MS - since_rx;
    if (q > need) need = q;
  }
  // Don't exceed max
  if (since_login + need > ROOM_SYNC_MAX_MS)
    need = ROOM_SYNC_MAX_MS - since_login;
  return need;
}

bool UITask::isRoomSyncing(const uint8_t* prefix6) const {
  return roomSyncRemainingMs(prefix6) > 0;
}

bool UITask::allowSendToContact(const ContactInfo& c, bool toast_if_blocked) {
  // Only gate room servers (backlog push is room-specific)
  if (c.type != ADV_TYPE_ROOM) return true;
  uint32_t rem = roomSyncRemainingMs(c.id.pub_key);
  if (rem == 0) return true;
  if (toast_if_blocked) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Syncing room... %us",
             (unsigned)((rem + 999) / 1000));
    toast(msg, C_YELLOW);
  }
  return false;
}

bool UITask::resyncRoom(const ContactInfo& c, bool full_history) {
  if (!mesh) return false;
  ContactInfo* live = mesh->lookupContactByPubKey(c.id.pub_key, 6);
  if (!live) {
    toast("Room not in contacts", C_RED);
    return false;
  }
  if (live->type != ADV_TYPE_ROOM) {
    toast("Not a room server", C_YELLOW);
    return false;
  }

  if (full_history) {
    mesh->resetRoomSyncSince(*live);
    termLog(C_TERM_SYS, "resync %s: full backlog requested", live->name);
  } else {
    termLog(C_TERM_SYS, "resync %s: re-login (keep sync_since)", live->name);
  }

  // Clear session flags so UI doesn't think we're done
  int idx = roomCredIndex(live->id.pub_key);
  if (idx >= 0) {
    _room_session_ok[idx] = 0;
    // Give resync its own auto-try budget
    _room_auto_tries[idx] = 0;
  }
  // Drop any in-progress sync window; fresh one starts on login OK
  int si = roomSyncIndex(live->id.pub_key);
  if (si >= 0) _room_sync[si].active = false;

  clearLoginPending("resync");

  const RoomCred* e = findRoomCred(live->id.pub_key);
  const char* pwd = e ? e->password : "";
  bool auto_on = e ? (e->auto_login != 0) : true;

  if (!beginRoomLogin(*live, pwd, auto_on, true /* force */)) {
    toast("Resync login failed", C_RED);
    return false;
  }
  toast(full_history ? "Resyncing full backlog..." : "Reconnecting to room...",
        C_CYAN);
  return true;
}

void UITask::saveRoomCreds() {
  File f = SPIFFS.open(ROOM_CRED_FILE, "w");
  if (!f) return;
  uint32_t magic = ROOM_CRED_MAGIC;
  f.write((uint8_t*)&magic, 4);
  f.write((uint8_t*)&_room_cred_n, 4);
  for (int i = 0; i < _room_cred_n; i++)
    f.write((uint8_t*)&_room_creds[i], sizeof(RoomCred));
  f.close();
}

const UITask::RoomCred* UITask::findRoomCred(const uint8_t* prefix6) const {
  if (!prefix6) return nullptr;
  for (int i = 0; i < _room_cred_n; i++)
    if (memcmp(_room_creds[i].pub_prefix, prefix6, 6) == 0)
      return &_room_creds[i];
  return nullptr;
}

UITask::RoomCred* UITask::findRoomCredMut(const uint8_t* prefix6) {
  return const_cast<RoomCred*>(findRoomCred(prefix6));
}

void UITask::saveRoomCred(const ContactInfo& c, const char* password, bool auto_login) {
  if (!password) return;
  int idx = roomCredIndex(c.id.pub_key);
  RoomCred* e = nullptr;
  if (idx < 0) {
    if (_room_cred_n >= ROOM_CRED_MAX) {
      memmove(&_room_creds[0], &_room_creds[1],
              sizeof(RoomCred) * (ROOM_CRED_MAX - 1));
      memmove(&_room_session_ok[0], &_room_session_ok[1],
              sizeof(_room_session_ok) - 1);
      memmove(&_room_auto_tries[0], &_room_auto_tries[1],
              sizeof(_room_auto_tries) - 1);
      _room_cred_n = ROOM_CRED_MAX - 1;
    }
    idx = _room_cred_n++;
    e = &_room_creds[idx];
    memset(e, 0, sizeof(*e));
    memcpy(e->pub_prefix, c.id.pub_key, 6);
    _room_session_ok[idx] = 0;
    _room_auto_tries[idx] = 0;
  } else {
    e = &_room_creds[idx];
  }
  StrHelper::strncpy(e->password, password, sizeof(e->password));
  e->auto_login = auto_login ? 1 : 0;
  e->type = c.type;
  StrHelper::strncpy(e->name, c.name, sizeof(e->name));
  saveRoomCreds();
  termLog(C_TERM_SYS, "saved login for %s (auto=%d)", e->name, e->auto_login ? 1 : 0);
}

void UITask::forgetRoomCred(const uint8_t* prefix6) {
  for (int i = 0; i < _room_cred_n; i++) {
    if (memcmp(_room_creds[i].pub_prefix, prefix6, 6) == 0) {
      memmove(&_room_creds[i], &_room_creds[i + 1],
              sizeof(RoomCred) * (_room_cred_n - i - 1));
      memmove(&_room_session_ok[i], &_room_session_ok[i + 1],
              (size_t)(_room_cred_n - i - 1));
      memmove(&_room_auto_tries[i], &_room_auto_tries[i + 1],
              (size_t)(_room_cred_n - i - 1));
      _room_cred_n--;
      saveRoomCreds();
      return;
    }
  }
}

bool UITask::setRoomAutoLogin(const uint8_t* prefix6, bool on) {
  RoomCred* e = findRoomCredMut(prefix6);
  if (!e) return false;
  e->auto_login = on ? 1 : 0;
  saveRoomCreds();
  return true;
}

bool UITask::beginRoomLogin(const ContactInfo& c, const char* password,
                            bool auto_login_if_ok, bool force) {
  if (!mesh || !password) return false;

  // Already keep-alive connected — treat as success so UI leaves password screen
  // (unless force=true for resync)
  if (!force && mesh->isLoggedInto(c.id.pub_key)) {
    markRoomSessionOk(c.id.pub_key);
    termLog(C_TERM_SYS, "login skip %s (already connected)", c.name);
    toast("Already logged in", C_GREEN);
    return true;
  }
  if (force && mesh->isLoggedInto(c.id.pub_key)) {
    mesh->endServerSession(c.id.pub_key);
  }

  // Expire stuck "in flight" state (no RESPONSE / never matched) so retries work
  if (_login_pending_valid && _login_pending_ms &&
      (int32_t)(millis() - _login_pending_ms) > (int32_t)LOGIN_PENDING_TIMEOUT_MS) {
    clearLoginPending("timeout");
  }

  if (_login_pending_valid) {
    // Same room: allow re-send (previous attempt may have been lost on air)
    if (memcmp(_login_pending_prefix, c.id.pub_key, 6) == 0) {
      clearLoginPending("retry same room");
    } else {
      termLog(C_TERM_SYS, "login skip %s (another login in flight)", c.name);
      toast("Login already in progress", C_YELLOW);
      return false;
    }
  }

  // Stale session_ok without mesh connection: allow re-login (force clear flag)
  {
    int idx = roomCredIndex(c.id.pub_key);
    if (idx >= 0) _room_session_ok[idx] = 0;
  }

  memcpy(_login_pending_prefix, c.id.pub_key, 6);
  StrHelper::strncpy(_login_pending_pwd, password, sizeof(_login_pending_pwd));
  _login_pending_auto = auto_login_if_ok;
  _login_pending_valid = true;
  _login_pending_ms = millis();
  uint32_t est = 0;
  int res = mesh->loginWithPassword(c, password, est);
  if (res == MSG_SEND_FAILED) {
    clearLoginPending("send failed");
    toast("Login send failed", C_RED);
    return false;
  }
  char msg[48];
  snprintf(msg, sizeof(msg), "Logging in to %s...", c.name);
  toast(msg, C_CYAN);
  termLog(C_TERM_TX, "[login->%s] pwd_len=%u", c.name, (unsigned)strlen(password));
  repLog(">", "login sent...");
  return true;
}

void UITask::tryAutoLoginRooms() {
  if (!mesh || _room_cred_n == 0) return;

  // Don't block forever if a previous attempt never completed
  // (try already counted when the TX was started)
  if (_login_pending_valid && _login_pending_ms &&
      (int32_t)(millis() - _login_pending_ms) > (int32_t)LOGIN_PENDING_TIMEOUT_MS) {
    clearLoginPending("auto-login timeout");
  }
  if (_login_pending_valid) return;  // wait for current attempt

  // One room at a time (MeshCore has a single pending_login)
  bool waiting_for_contact = false;
  for (int i = 0; i < _room_cred_n; i++) {
    if (!roomNeedsAutoLogin(i)) continue;
    ContactInfo* live = mesh->lookupContactByPubKey(_room_creds[i].pub_prefix, 6);
    if (!live) {
      waiting_for_contact = true;
      continue;
    }
    const uint8_t max_tries = autoLoginMaxTries();
    // Count this TX attempt before send
    if (_room_auto_tries[i] >= max_tries) continue;
    _room_auto_tries[i]++;
    termLog(C_TERM_TX, "auto-login try %u/%u -> %s",
            (unsigned)_room_auto_tries[i], (unsigned)max_tries,
            _room_creds[i].name[0] ? _room_creds[i].name : live->name);
    if (beginRoomLogin(*live, _room_creds[i].password, true)) {
      // If already connected, beginRoomLogin returns true without pending —
      // don't leave a dead retry schedule for this room
      if (mesh->isLoggedInto(live->id.pub_key)) {
        _room_session_ok[i] = 1;
      }
    } else {
      // Send failed / busy — leave try counted; schedule another room later
      _auto_login_at = millis() + 2500;
    }
    return;
  }
  // Contact not in book yet: retry a few times then stop (not forever)
  if (waiting_for_contact) {
    const uint8_t max_tries = autoLoginMaxTries();
    if (_auto_login_wait_rounds < max_tries) {
      _auto_login_wait_rounds++;
      termLog(C_TERM_SYS, "auto-login: waiting for room contact (%u/%u)",
              (unsigned)_auto_login_wait_rounds, (unsigned)max_tries);
      _auto_login_at = millis() + 5000;
    } else {
      termLog(C_TERM_SYS, "auto-login: gave up waiting for room contacts");
      _auto_login_at = 0;
    }
    return;
  }
  _auto_login_at = 0;
}

void UITask::onLoginResult(const ContactInfo& from, bool ok) {
  termLog(ok ? C_TERM_RX : C_TERM_ERR, "[%s] login %s", from.name, ok ? "OK" : "FAIL");
  // Clear "waiting for reply..." with a definitive console line
  repLog(from.name, ok ? "LOGIN OK - session active" : "LOGIN FAILED");

  bool match = _login_pending_valid &&
               memcmp(_login_pending_prefix, from.id.pub_key, 6) == 0;
  // Also accept 4-byte prefix match (mesh pending_login is only 4 bytes)
  if (!match && _login_pending_valid &&
      memcmp(_login_pending_prefix, from.id.pub_key, 4) == 0)
    match = true;

  if (ok) {
    // Always persist credentials on successful pending login (blank pwd OK)
    if (match) {
      saveRoomCred(from, _login_pending_pwd, _login_pending_auto);
    }
    markRoomSessionOk(from.id.pub_key);
    // If no cred slot yet (history path without pending), still try mark
    if (!findRoomCred(from.id.pub_key) && match) {
      saveRoomCred(from, _login_pending_pwd, _login_pending_auto);
      markRoomSessionOk(from.id.pub_key);
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "Logged in: %s", from.name);
    toast(msg, C_GREEN);
    // Room servers push backlog stop-and-wait — block TX until quiet
    if (from.type == ADV_TYPE_ROOM)
      beginRoomSync(from.id.pub_key);
  } else {
    toast("Login failed", C_RED);
  }

  // Always release UI pending on match so password screen can retry
  if (match) clearLoginPending(ok ? "ok" : "fail");

  // Notify repeaters console if open
  {
    RepeatersScreen* rs = (RepeatersScreen*)_screens[SCR_REPEATERS];
    if (rs) rs->onLoginFinished(from.name, ok);
  }

  // Schedule next auto-login only if some room still has tries left
  bool more = false;
  for (int i = 0; i < _room_cred_n; i++) {
    if (roomNeedsAutoLogin(i)) { more = true; break; }
  }
  if (more) {
    _auto_login_at = millis() + 2500;
  } else {
    _auto_login_at = 0;
    // Log if any room exhausted tries without session
    const uint8_t max_tries = autoLoginMaxTries();
    for (int i = 0; i < _room_cred_n; i++) {
      if (_room_creds[i].auto_login && !_room_session_ok[i] &&
          _room_auto_tries[i] >= max_tries) {
        termLog(C_TERM_SYS, "auto-login stopped for %s after %u tries",
                _room_creds[i].name[0] ? _room_creds[i].name : "?",
                (unsigned)max_tries);
      }
    }
  }
  _dirty = true;
}

// ---------------------------------------------------------------- WiFi

void UITask::loadWifi() {
  File f = SPIFFS.open("/wifi.cfg", "r");
  if (!f) return;
  String s = f.readStringUntil('\n'); s.trim();
  String p = f.readStringUntil('\n'); p.trim();
  String w = f.readStringUntil('\n');
  StrHelper::strncpy(_wifi_ssid, s.c_str(), sizeof(_wifi_ssid));
  StrHelper::strncpy(_wifi_pass, p.c_str(), sizeof(_wifi_pass));
  _wifi_want = w.toInt() != 0;
  f.close();
}

void UITask::saveWifi() {
  File f = SPIFFS.open("/wifi.cfg", "w");
  if (!f) return;
  f.printf("%s\n%s\n%d\n", _wifi_ssid, _wifi_pass, _wifi_want ? 1 : 0);
  f.close();
}

void UITask::wifiConnect() {
  if (_wifi_ssid[0] == 0) { toast("Set a WiFi SSID first", C_YELLOW); return; }
  _wifi_want = true;
  saveWifi();
  WiFi.mode(WIFI_STA);
  WiFi.begin(_wifi_ssid, _wifi_pass);
  toast("WiFi: connecting...", C_CYAN);
  termLog(C_TERM_SYS, "WiFi connecting to %s", _wifi_ssid);
}

void UITask::wifiOff() {
  _wifi_want = false;
  saveWifi();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  toast("WiFi off", C_YELLOW);
}

int UITask::wifiState() const {
  if (!_wifi_want) return 0;
  return WiFi.status() == WL_CONNECTED ? 2 : 1;
}

// Fetch a .mdm map pack over WiFi (from the flasher site) onto the SD card.
const char* UITask::downloadMapPack(const char* name) {
  if (wifiState() != 2) return "Connect WiFi first";
  char url[128];
  snprintf(url, sizeof(url), "https://meshdeck-os.github.io/meshdeck/maps/%s.mdm", name);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return "URL error";
  int code = http.GET();
  if (code != 200) { http.end(); return "HTTP error"; }
  if (!hw.sdBegin()) { http.end(); return "No SD card"; }
  SD.mkdir("/meshdeck-maps");
  char path[48]; snprintf(path, sizeof(path), "/meshdeck-maps/%s.mdm", name);
  if (SD.exists(path)) SD.remove(path);   // avoid stale trailing bytes on re-download
  File f = SD.open(path, FILE_WRITE);
  if (!f) { http.end(); hw.sdEnd(); return "SD write failed"; }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  int total = 0, len = http.getSize();
  uint32_t t0 = millis();
  while (http.connected() && (len > 0 || len == -1) && millis() - t0 < 30000) {
    size_t avail = stream->available();
    if (avail) {
      int r = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      f.write(buf, r); total += r;
      if (len > 0) { len -= r; if (len == 0) break; }
    } else delay(2);
  }
  f.close(); http.end(); hw.sdEnd();
  if (total <= 0) return "Empty download";
  reloadSDMaps();
  return nullptr;
}

// Fetch the list of available map packs (maps/index.txt) over WiFi.
const char* UITask::downloadMapIndex(char* buf, size_t sz, int& outlen) {
  outlen = 0;
  if (wifiState() != 2) return "Connect WiFi first";
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  const char* url = "https://meshdeck-os.github.io/meshdeck/maps/index.txt";
  if (!http.begin(client, url)) return "URL error";
  int code = http.GET();
  if (code != 200) { http.end(); return "HTTP error"; }
  String body = http.getString();
  http.end();
  int n = body.length();
  if (n <= 0) return "Empty index";
  if (n > (int)sz - 1) n = sz - 1;
  memcpy(buf, body.c_str(), n);
  buf[n] = 0;
  outlen = n;
  return nullptr;
}

const char* UITask::prepareSD() {
  if (!hw.sdBegin()) return "No SD card";
  bool ok = SD.exists("/meshdeck-maps") || SD.mkdir("/meshdeck-maps");
  hw.sdEnd();
  return ok ? nullptr : "SD error";
}

// ---------------------------------------------------------------- Onboarding screen

struct RadioPreset { const char* name; float freq; float bw; uint8_t sf; uint8_t cr; };
static const RadioPreset PRESETS[] = {
  { "EU/UK (Narrow)",      869.618f, 62.5f,   8, 8 },
  { "EU/UK (Deprecated)",  869.525f, 250.0f, 11, 5 },
  { "Netherlands",         869.618f, 62.5f,   7, 5 },
  { "Czech (Narrow)",      869.432f, 62.5f,   7, 5 },
  { "Switzerland",         869.618f, 62.5f,   8, 8 },
  { "Portugal 868",        869.618f, 62.5f,   7, 6 },
  { "EU 433 (Narrow)",     433.650f, 62.5f,   8, 8 },
  { "EU 433 (LongRange)",  433.650f, 250.0f, 11, 5 },
  { "USA / Canada",        910.525f, 62.5f,   7, 5 },
  { "Australia",           915.800f, 250.0f, 10, 5 },
  { "Australia (Narrow)",  916.575f, 62.5f,   7, 8 },
  { "New Zealand",         917.375f, 250.0f, 11, 5 },
  { "New Zealand (Narrow)",917.375f, 62.5f,   7, 5 },
  { "Vietnam (Narrow)",    920.250f, 62.5f,   8, 5 },
};
#define N_PRESETS ((int)(sizeof(PRESETS)/sizeof(PRESETS[0])))
#define ONB_TOTAL (N_PRESETS + 1)          // + "Keep current"
#define ONB_ROW_H 26
#define ONB_TOP   (STATUS_H + 6)
#define ONB_VIS   ((SCREEN_H - ONB_TOP - 4) / ONB_ROW_H)

void OnboardScreen::enter() {
  _phase = 0;
  StrHelper::strncpy(_name, ui.prefs ? ui.prefs->node_name : "", sizeof(_name));
  _nlen = strlen(_name);
  _sel = 0; _top = 0;
}
void OnboardScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  c.setTextSize(1);

  // ---- phase 0: enter node name ----
  if (_phase == 0) {
    ui.drawStatusBar("Welcome - set your name");
    c.setTextColor(C_FG_DIM);
    c.setCursor(20, 56);
    c.print("This is how other nodes see you.");

    int bx = 20, by = 86, boxw = SCREEN_W - 40, bh = 26;
    c.fillRoundRect(bx, by, boxw, bh, 5, C_BG_RAISED);
    c.drawRoundRect(bx, by, boxw, bh, 5, C_ACCENT);
    _name[_nlen] = 0;
    c.setTextColor(C_FG);
    c.setCursor(bx + 8, by + 9);
    c.print(_name);
    c.fillRect(bx + 8 + _nlen * 6 + 1, by + 7, 2, 12, C_ACCENT);   // cursor

    c.setTextColor(C_FG_FAINT);
    c.setCursor(20, SCREEN_H - 20);
    c.print("type a name, enter = next");
    return;
  }

  // ---- phase 1: radio preset list ----
  ui.drawStatusBar("Select Radio Preset");

  if (_sel < _top) _top = _sel;
  if (_sel >= _top + ONB_VIS) _top = _sel - ONB_VIS + 1;

  for (int i = _top; i < ONB_TOTAL && i < _top + ONB_VIS; i++) {
    int y = ONB_TOP + (i - _top) * ONB_ROW_H;
    bool sel = i == _sel;
    if (sel) c.fillRoundRect(2, y - 1, SCREEN_W - 4, ONB_ROW_H - 2, 5, C_BG_RAISED);
    if (i < N_PRESETS) {
      const RadioPreset& p = PRESETS[i];
      c.setTextColor(sel ? C_FG : C_FG_DIM);
      c.setCursor(10, y + 2);
      c.print(p.name);
      char params[48];
      char fq[16];
      snprintf(fq, sizeof(fq), "%.3f", p.freq);
      snprintf(params, sizeof(params), "%s MHz  SF%d  BW%.1f  CR%d", fq, (int)p.sf, p.bw, (int)p.cr);
      c.setTextColor(C_FG_FAINT);
      c.setCursor(10, y + 13);
      c.print(params);
    } else {
      c.setTextColor(sel ? C_ACCENT : C_FG_DIM);
      c.setCursor(10, y + 6);
      c.print("Keep current settings");
    }
  }

  // scrollbar
  if (ONB_TOTAL > ONB_VIS) {
    int bar_h = (SCREEN_H - ONB_TOP) * ONB_VIS / ONB_TOTAL;
    int bar_y = ONB_TOP + (SCREEN_H - ONB_TOP - bar_h) * _top / (ONB_TOTAL - ONB_VIS);
    c.fillRect(SCREEN_W - 3, bar_y, 2, bar_h, C_FG_FAINT);
  }
}

void OnboardScreen::choose(int i) {
  // save the node name entered in phase 0
  if (_nlen > 0 && ui.prefs && ui.mesh) {
    _name[_nlen] = 0;
    StrHelper::strncpy(ui.prefs->node_name, _name, sizeof(ui.prefs->node_name));
    ui.mesh->savePrefs();
  }
  if (i >= 0 && i < N_PRESETS) {
    const RadioPreset& p = PRESETS[i];
    ui.applyPreset(p.freq, p.bw, p.sf, p.cr);
    char buf[44];
    snprintf(buf, sizeof(buf), "%s selected", p.name);
    ui.toast(buf, C_GREEN);
  } else {
    ui.toast("Keeping current radio settings", C_YELLOW);
  }
  ui.finishOnboarding();
}


bool OnboardScreen::key(uint8_t k) {
  if (_phase == 0) {
    if (k == 0x0D) { if (_nlen > 0) { _phase = 1; _sel = 0; _top = 0; } return true; }
    if (k == 0x08 || k == 0x7F) { if (_nlen > 0) _nlen--; return true; }
    if (k >= 32 && k < 127 && _nlen < (int)sizeof(_name) - 2) { _name[_nlen++] = k; return true; }
    return true;
  }
  if (k == 0x0D) { choose(_sel); return true; }
  return false;
}

bool OnboardScreen::nav(NavEvent e) {
  if (_phase == 0) {
    if (e == NAV_SELECT && _nlen > 0) { _phase = 1; _sel = 0; _top = 0; }
    return true;   // no list navigation during name entry
  }
  switch (e) {
    case NAV_UP:     if (_sel > 0) _sel--; return true;
    case NAV_DOWN:   if (_sel < ONB_TOTAL - 1) _sel++; return true;
    case NAV_SELECT: choose(_sel); return true;
    case NAV_BACK:   _phase = 0; return true;   // back to the name step
    default: return true;
  }
}

bool OnboardScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  if (_phase == 0) { if (_nlen > 0) { _phase = 1; _sel = 0; _top = 0; } return true; }
  int idx = _top + (e.y - ONB_TOP) / ONB_ROW_H;
  if (idx >= 0 && idx < ONB_TOTAL) {
    if (idx == _sel) choose(idx);
    else _sel = idx;
  }
  return true;
}

// ---------------------------------------------------------------- Radio diagnostics screen

void DiagScreen::enter() { _page = 0; }

void DiagScreen::refreshStorage() {
  _flash_tot_kb  = SPIFFS.totalBytes() / 1024;
  _flash_used_kb = SPIFFS.usedBytes()  / 1024;
  _sd_present = false;
  if (ui.hw.sdBegin()) {                       // grabs the shared SPI bus briefly
    uint64_t tot = SD.totalBytes(), used = SD.usedBytes();
    if (tot > 0) { _sd_present = true; _sd_tot_mb = tot / (1024*1024); _sd_free_mb = (tot - used) / (1024*1024); }
    ui.hw.sdEnd();
  }
}

void DiagScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  c.setTextSize(1);

  // rolling free-RAM history, sampled once per redraw (~1/s on this screen) (#3)
  static const int RAMN = 116;
  static uint16_t ramHist[RAMN];
  ramHist[_ram_head] = (uint16_t)(ESP.getFreeHeap() / 1024);
  _ram_head = (_ram_head + 1) % RAMN;

  if (_page == 1) {                              // ---- page 2: storage + graph (#3)
    ui.drawStatusBar("Storage & Performance");
    int y2 = STATUS_H + 10; char sv[48];
    c.setTextColor(C_ACCENT); c.setCursor(8, y2); c.print("Storage"); y2 += 15;

    c.setTextColor(C_FG_DIM); c.setCursor(12, y2); c.print("Flash (SPIFFS)");
    snprintf(sv, sizeof(sv), "%u / %u KB", (unsigned)_flash_used_kb, (unsigned)_flash_tot_kb);
    c.setTextColor(C_FG); c.setCursor(170, y2); c.print(sv); y2 += 14;

    c.setTextColor(C_FG_DIM); c.setCursor(12, y2); c.print("SD card");
    if (_sd_present) { snprintf(sv, sizeof(sv), "%u free / %u MB", (unsigned)_sd_free_mb, (unsigned)_sd_tot_mb); c.setTextColor(C_GREEN); }
    else             { snprintf(sv, sizeof(sv), "no card"); c.setTextColor(C_FG_FAINT); }
    c.setCursor(170, y2); c.print(sv); y2 += 14;

    c.setTextColor(C_FG_DIM); c.setCursor(12, y2); c.print("CPU clock");
    snprintf(sv, sizeof(sv), "%u MHz", (unsigned)getCpuFrequencyMhz());
    c.setTextColor(C_FG); c.setCursor(170, y2); c.print(sv); y2 += 18;

    c.setTextColor(C_ACCENT); c.setCursor(8, y2); c.print("Free RAM (KB)"); y2 += 14;
    const int GX = 34, GY = y2, GW = SCREEN_W - GX - 8, GH = SCREEN_H - GY - 22;
    c.drawRect(GX, GY, GW, GH, C_FG_FAINT);
    uint16_t mn = 0xFFFF, mx = 0;
    for (int i = 0; i < RAMN; i++) { uint16_t vv = ramHist[i]; if (!vv) continue; if (vv < mn) mn = vv; if (vv > mx) mx = vv; }
    if (!mx) { c.setTextColor(C_FG_FAINT); c.setCursor(GX + 6, GY + GH / 2); c.print("sampling..."); }
    else {
      if (mx == mn) mx = mn + 1;
      int px = -1, py = -1;
      for (int i = 0; i < RAMN; i++) {
        uint16_t val = ramHist[(_ram_head + i) % RAMN];
        if (!val) continue;
        int x = GX + i * GW / RAMN;
        int yy = GY + GH - (int)((long)(val - mn) * GH / (mx - mn));
        if (yy < GY) yy = GY; if (yy > GY + GH) yy = GY + GH;
        if (px >= 0) c.drawLine(px, py, x, yy, C_ACCENT);
        px = x; py = yy;
      }
      char lbl[8];
      c.setTextColor(C_FG_FAINT);
      snprintf(lbl, sizeof(lbl), "%u", (unsigned)mx); c.setCursor(4, GY - 3);        c.print(lbl);
      snprintf(lbl, sizeof(lbl), "%u", (unsigned)mn); c.setCursor(4, GY + GH - 6);    c.print(lbl);
    }
    c.setTextColor(C_FG_FAINT); c.setCursor(6, SCREEN_H - 10); c.print("roll left = back to radio");
    return;
  }

  ui.drawStatusBar("Radio Diagnostics");
  NodePrefs* p = ui.prefs;
  int y = STATUS_H + 10;
  char v[40];

  // radio profile
  c.setTextColor(C_ACCENT); c.setCursor(8, y); c.print("Radio profile"); y += 15;

  char fq[16]; snprintf(fq, sizeof(fq), "%.3f", p ? p->freq : 0);
  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Frequency");
  snprintf(v, sizeof(v), "%s MHz", fq);
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 14;

  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("SF / BW / CR");
  snprintf(v, sizeof(v), "SF%d  %.1f  CR%d", p ? (int)p->sf : 0, p ? p->bw : 0, p ? (int)p->cr : 0);
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 14;

  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("TX power");
  snprintf(v, sizeof(v), "%d dBm", p ? (int)p->tx_power_dbm : 0);
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 18;

  // receive activity
  c.setTextColor(C_ACCENT); c.setCursor(8, y); c.print("Receive"); y += 15;

  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Last packet");
  if (ui.rxCount() == 0) {
    c.setTextColor(C_RED); c.setCursor(150, y); c.print("never heard");
  } else {
    uint32_t age = (millis() - ui.lastRxMillis()) / 1000;
    snprintf(v, sizeof(v), "%us ago", age);
    c.setTextColor(age < 60 ? C_GREEN : age < 300 ? C_YELLOW : C_FG_FAINT);
    c.setCursor(150, y); c.print(v);
  }
  y += 14;

  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("RSSI / SNR");
  snprintf(v, sizeof(v), "%d dBm  %s", (int)ui.lastRxRssi(), StrHelper::ftoa(ui.lastRxSnr()));
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 14;

  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Packets RX");
  snprintf(v, sizeof(v), "%u", ui.rxCount());
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 18;

  // mesh
  c.setTextColor(C_ACCENT); c.setCursor(8, y); c.print("Mesh"); y += 15;
  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Contacts / heard");
  snprintf(v, sizeof(v), "%d / %d", (int)(ui.mesh ? ui.mesh->getNumContacts() : 0), (int)ui.heardCount());
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 18;

  // system performance (#3)
  c.setTextColor(C_ACCENT); c.setCursor(8, y); c.print("System"); y += 15;
  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("RAM / PSRAM free");
  snprintf(v, sizeof(v), "%u / %u KB", (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 14;
  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Uptime");
  uint32_t up = millis() / 1000;
  snprintf(v, sizeof(v), "%uh %02um %02us", up / 3600, (up % 3600) / 60, up % 60);
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v);

  c.setTextColor(C_FG_FAINT); c.setCursor(6, SCREEN_H - 10);
  c.print("A=advert     roll right = storage & graph");
}

bool DiagScreen::key(uint8_t k) {
  if (_page == 0 && (k == 'a' || k == 'A')) {
    if (ui.mesh) ui.mesh->advertFlood();
    ui.toast("Advert sent", C_CYAN);
    return true;
  }
  return false;
}

bool DiagScreen::nav(NavEvent e) {
  if (e == NAV_RIGHT && _page == 0) { _page = 1; refreshStorage(); return true; }
  if (e == NAV_LEFT  && _page == 1) { _page = 0; return true; }
  return false;   // BACK (hold) still propagates to leave the screen
}

// ---------------------------------------------------------------- SOS beacon screen

void SOSScreen::draw() {
  GFXcanvas16& c = ui.cv();
  bool on = ui.sosActive();
  c.fillScreen(on ? C_RED : C_BG);
  ui.drawStatusBar("SOS Beacon");

  c.setTextSize(3);
  c.setTextColor(on ? 0xFFFF : C_RED);
  c.setCursor(120, 60);
  c.print("SOS");

  c.setTextSize(1);
  c.setTextColor(on ? 0xFFFF : C_FG_DIM);
  c.setCursor(40, 108);
  c.print(on ? "ACTIVE - broadcasting every 2 min" : "Emergency location beacon");

  double lat, lon;
  bool have_pos = ui.ownPos(lat, lon);
  char loc[52];
  if (have_pos) snprintf(loc, sizeof(loc), "pos: %.5f, %.5f", lat, lon);
  else snprintf(loc, sizeof(loc), "no location - set GPS or manual in Settings");
  c.setTextColor(on ? 0xFFFF : (have_pos ? C_FG_FAINT : C_YELLOW));
  c.setCursor(40, 132);
  c.print(loc);

  c.setTextColor(on ? 0xFFFF : (have_pos ? C_ACCENT : C_FG_FAINT));
  c.setCursor(70, 190);
  c.print(on ? "Click to STOP" : have_pos ? "Click to START" : "Needs a location first");
}

bool SOSScreen::key(uint8_t k) {
  if (k == 0x0D) { ui.toggleSOS(); return true; }
  return false;
}

bool SOSScreen::nav(NavEvent e) {
  if (e == NAV_SELECT) { ui.toggleSOS(); return true; }
  return false;   // let BACK bubble up to leave the screen
}

bool SOSScreen::touch(const TouchEvent& e) {
  if (e.kind == TouchEvent::TAP) { ui.toggleSOS(); return true; }
  return false;
}
