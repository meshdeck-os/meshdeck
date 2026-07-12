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

#define SETTINGS_FILE "/meshdeck_set.bin"

// ---------------------------------------------------------------- text utils

void ellipsize(char* dst, size_t dst_sz, const char* src) {
  size_t n = strlen(src);
  if (n < dst_sz) { strcpy(dst, src); return; }
  size_t keep = dst_sz - 3;
  memcpy(dst, src, keep);
  dst[keep] = dst[keep + 1] = '.';
  dst[keep + 2] = 0;
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
  set.timeout_s = 0;          // never auto-sleep by default (opt-in in Settings)
  set.sounds = 1;
  set.volume = 6;
  set.flip = 0;
  set.always_on = 0;
  set.man_lat = set.man_lon = 0;
  set.touch_map = 2;          // correct T-Deck landscape mapping (swap XY + mirror)

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
  File f = SPIFFS.open(SETTINGS_FILE, "r");
  if (f) {
    DeckSettings tmp;
    if (f.read((uint8_t*)&tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == DECKSET_MAGIC) set = tmp;
    f.close();
  }
  // Migrate old saves off the wrong touch mapping: map 0 (landscape-direct) is
  // never correct on the T-Deck GT911, so treat a stored 0 as "use the default".
  if (set.touch_map == 0) set.touch_map = 2;
  applySettings();

  store.begin();

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

  termLog(C_TERM_SYS, "MeshDeck v%s on MeshCore %s", MESHDECK_VERSION, FIRMWARE_VERSION);
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

  hw.chimeBoot();
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
  if (_stack_len < 8) _stack[_stack_len++] = _cur;
  _cur = id;
  _screens[_cur]->enter();
  _dirty = true;
}

void UITask::back() {
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
  _stack_len = 0;
  _cur = SCR_HOME;
  _screens[_cur]->enter();
  _dirty = true;
}

void UITask::toast(const char* msg, uint16_t color) {
  StrHelper::strncpy(_toast, msg, sizeof(_toast));
  _toast_color = color;
  _toast_until = millis() + 2600;
  _dirty = true;
}

void UITask::openThread(int thread_idx) {
  _pending_thread = thread_idx;
  if (_cur != SCR_CHAT) go(SCR_CHAT);
  else { _screens[SCR_CHAT]->enter(); _dirty = true; }
}

void UITask::openQR(const char* url) {
  StrHelper::strncpy(_qr_url, url, sizeof(_qr_url));
  go(SCR_QR);
}

// ---------------------------------------------------------------- hooks

void UITask::notify(UIEventType t) {
  _dirty = true;
}

void UITask::onContactMsg(const ContactInfo& from, const char* text, uint32_t sender_ts,
                          uint8_t path_len, float snr) {
  DeckThread* t = store.forContact(from.id.pub_key, from.name);
  if (t) {
    store.addMsg(t, from.name, text, epochNow(), 0, (int8_t)(snr * 4), path_len, 0);
  }
  termLog(C_TERM_RX, "[DM] %s: %s", from.name, text);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s: %.24s", from.name, text);
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

void UITask::onChannelMsg(uint8_t channel_idx, const char* channel_name, const char* text,
                          uint32_t ts, uint8_t path_len, float snr) {
  // MeshCore convention: channel text is "SenderName: message"
  char sender[MD_SENDER_LEN];
  const char* body = strchr(text, ':');
  if (body && body - text < MD_SENDER_LEN + 12) {
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
  DeckThread* t = store.forChannel(channel_idx, channel_name);
  if (t) {
    store.addMsg(t, sender, body, epochNow(), 0, (int8_t)(snr * 4), path_len, 0);
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
          contact.type == ADV_TYPE_REPEATER ? "repeater" : contact.type == ADV_TYPE_ROOM ? "room" : "chat",
          is_new ? ", new" : "");
  if (is_new) {
    char buf[48];
    snprintf(buf, sizeof(buf), "New node: %s", contact.name);
    toast(buf, C_PURPLE);
  }
  _dirty = true;
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

void UITask::repLog(const char* from, const char* text) {
  RepeatersScreen* r = (RepeatersScreen*)_screens[SCR_REPEATERS];
  if (r) r->onCliResponse(from, text);
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

void UITask::fmtClock(char* out, size_t sz) const {
  uint32_t e = epochNow();
  if (e < 1000000000) { snprintf(out, sz, "--:--"); return; }
  DateTime dt(e);
  snprintf(out, sz, "%02d:%02d", dt.hour(), dt.minute());
}

void UITask::fmtAgo(char* out, size_t sz, uint32_t then) const {
  uint32_t now = epochNow();
  if (then == 0 || then > now) { snprintf(out, sz, "-"); return; }
  uint32_t d = now - then;
  if (d < 60) snprintf(out, sz, "%us", d);
  else if (d < 3600) snprintf(out, sz, "%um", d / 60);
  else if (d < 86400) snprintf(out, sz, "%uh", d / 3600);
  else snprintf(out, sz, "%ud", d / 86400);
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
    _screens[_cur]->tick1s();
    if ((_cur == SCR_HOME || _cur == SCR_DIAG) && hw.isDisplayOn()) _dirty = true;
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
  if (set.timeout_s == 0 || set.always_on) return;
  uint32_t idle = millis() - hw.lastActivityMillis();
  if (hw.isDisplayOn() && idle > (uint32_t)set.timeout_s * 1000) {
    hw.displayOff();
  }
}

void UITask::dispatchInput() {
  // any input wakes the display
  uint8_t k = hw.readKey();
  NavEvent nv = hw.readNav();
  TouchEvent te;
  bool has_touch = hw.readTouch(te);

  if (!hw.isDisplayOn()) {
    if (k || nv != NAV_NONE || has_touch) {
      hw.displayOn();
      _dirty = true;
    }
    return;
  }

  Screen* s = _screens[_cur];
  bool used = false;

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
  _sos_active = !_sos_active;
  if (_sos_active) {
    hw.chimeError();
    _sos_last = 0;          // force an immediate first broadcast
    sendSOSNow();
    toast("SOS ACTIVE - broadcasting", C_RED);
  } else {
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

void OnboardScreen::choose(int i) {
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

void OnboardScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Select Radio Preset");
  c.setTextSize(1);
  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, STATUS_H + 4);
  (void)0;

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

bool OnboardScreen::key(uint8_t k) {
  if (k == 0x0D) { choose(_sel); return true; }
  return false;
}

bool OnboardScreen::nav(NavEvent e) {
  switch (e) {
    case NAV_UP:     if (_sel > 0) _sel--; return true;
    case NAV_DOWN:   if (_sel < ONB_TOTAL - 1) _sel++; return true;
    case NAV_SELECT: choose(_sel); return true;
    default: return true;   // swallow BACK - a choice is required
  }
}

bool OnboardScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  int idx = _top + (e.y - ONB_TOP) / ONB_ROW_H;
  if (idx >= 0 && idx < ONB_TOTAL) {
    if (idx == _sel) choose(idx);
    else _sel = idx;
  }
  return true;
}

// ---------------------------------------------------------------- Radio diagnostics screen

void DiagScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Radio Diagnostics");
  c.setTextSize(1);
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
  c.setTextColor(C_FG); c.setCursor(150, y); c.print(v); y += 16;

  c.setTextColor(C_FG_FAINT); c.setCursor(6, SCREEN_H - 10);
  c.print("press A to send a flood advert");
}

bool DiagScreen::key(uint8_t k) {
  if (k == 'a' || k == 'A') {
    if (ui.mesh) ui.mesh->advertFlood();
    ui.toast("Advert sent", C_CYAN);
    return true;
  }
  return false;
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
  char loc[52];
  if (ui.ownPos(lat, lon)) snprintf(loc, sizeof(loc), "pos: %.5f, %.5f", lat, lon);
  else snprintf(loc, sizeof(loc), "no GPS fix - set manual pos in Settings");
  c.setTextColor(on ? 0xFFFF : C_FG_FAINT);
  c.setCursor(40, 132);
  c.print(loc);

  c.setTextColor(on ? 0xFFFF : C_ACCENT);
  c.setCursor(90, 190);
  c.print(on ? "Click to STOP" : "Click to START");
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
