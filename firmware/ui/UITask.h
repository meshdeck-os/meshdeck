#pragma once
/*
 * MeshDeck UI orchestrator. Implements the AbstractUITask hook interface,
 * owns the hardware layer, message store, and all screens.
 */
#include <Arduino.h>
#include "../AbstractUITask.h"
#include "DeckHW.h"
#include "MessageStore.h"
#include "SDMap.h"
#include "NewMap.h"
#include "Theme.h"
#include <helpers/SensorManager.h>

class MyMesh;
class UITask;
class WebServer;   // ESP32 web server (remote screen), included only in WebScreen.cpp

enum ScreenId : uint8_t {
  SCR_HOME = 0, SCR_CHAT, SCR_CONTACTS, SCR_MAP, SCR_NEWMAPS, SCR_LASTHEARD, SCR_REPEATERS,
  SCR_TRACE, SCR_NOISE, SCR_TERMINAL, SCR_SETTINGS, SCR_QR,
  SCR_ONBOARD, SCR_DIAG, SCR_SOS, SCR_MAPDL, SCR_WIFI, SCR_CHANNELS, SCR_VOICE, SCR_COUNT
};

class Screen {
public:
  Screen(UITask& u) : ui(u) {}
  virtual void enter() {}
  /** Called when navigating away from this screen (before the next enter). */
  virtual void leave() {}
  virtual void draw() = 0;
  virtual bool key(uint8_t c) { return false; }
  virtual bool nav(NavEvent e) { return false; }
  virtual bool touch(const TouchEvent& e) { return false; }
  virtual void tick1s() {}
protected:
  UITask& ui;
};

// ---- persisted UI settings ----
#define DECKSET_MAGIC 0x4D444B53
struct DeckSettings {
  uint32_t magic;
  uint8_t brightness;      // 30..255
  uint16_t timeout_s;      // display sleep timeout (0 = never)
  uint8_t sounds;
  uint8_t volume;          // 0..10
  uint8_t flip;
  uint8_t always_on;       // keep clock screen lit
  int32_t man_lat, man_lon;   // manual position * 1e6 (0,0 = unset)
  uint8_t touch_map;          // 0..3, touch coordinate mapping
  uint8_t tb_speed;           // trackball speed 1(slow)..5(fast); 0 = unset -> default
  uint8_t configured;         // 0 = show first-boot radio-preset onboarding
  uint8_t adv_interval_min;   // auto-advert period in minutes (0 = off)
  int8_t  tz_offset;          // local time = UTC + tz_offset hours (-12..+14)
  uint8_t sos_disabled;       // 1 = hide/disable the SOS beacon tile
  uint8_t room_login_tries;   // auto-login attempts per room per boot (1..20, default 5)
  uint8_t reserved[1];
};

// ---- last heard ----
#define HEARD_MAX 32
struct HeardEntry {
  char name[24];
  uint8_t type;            // ADV_TYPE_*
  uint8_t hops;            // path len, 0xFF unknown
  int8_t snr4;
  int16_t rssi;
  uint32_t at;             // epoch
  int32_t lat, lon;        // * 1e6 (0,0 = none)
  uint8_t prefix[6];
};

// ---- terminal ----
#define TERM_LINES 160
#define TERM_COLS  84
struct TermLine { uint16_t color; char text[TERM_COLS]; };

// ---- noise history ----
#define NOISE_SAMPLES 320
// ---- ChatScreen layout constants (needed because draw() lives here) ----
#define TAB_H      16
#define INPUT_H    22
#define CHAT_TOP   (STATUS_H + 1 + TAB_H)
#define CHAT_BOT   (SCREEN_H - INPUT_H)
#define BUB_MAX_W  230


// ---- trace result ----
struct TraceResult {
  bool valid;
  uint32_t tag;
  char target[28];
  uint8_t hops;
  int8_t snrs[16];         // snr*4 per hop
  uint8_t hashes[16];
  float final_snr;
  uint32_t at_millis;
  uint32_t sent_millis;
};

class UITask : public AbstractUITask {
public:
  UITask(mesh::MainBoard* board, BaseSerialInterface* serial);

  void earlyInit();                    // display splash before radio init
  void bootStatus(const char* msg);    // update the splash progress line
  void fatalError(const char* msg);
  void begin(MyMesh* mesh, SensorManager* sensors, NodePrefs* prefs);

  // ---- AbstractUITask ----
  void msgRead(int msgcount) override {}
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override {}
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;

  // ---- rich hooks from MyMesh ----
  void reresolveThreadSenders(DeckThread* t);
  void onVoiceRecv(const mesh::GroupChannel& channel,
                   const uint8_t* voice_data, size_t len,
                   bool end_of_stream, float snr) override;

  void onVoiceRecvFromContact(const ContactInfo& from,
                              const uint8_t* voice_data, size_t len,
                              bool end_of_stream, float snr) override;

  void onVoicePacketSent(uint32_t tag, uint16_t len, bool eos) override;
  void onVoicePacketAcked(uint32_t tag, bool eos) override;

  void onContactMsg(const ContactInfo& from, const char* text, uint32_t sender_ts,
                  uint8_t path_len, float snr,
                  const uint8_t* sender_prefix = nullptr) override;
  void onCliResponse(const ContactInfo& from, const char* text) override;
  void onChannelMsg(uint8_t channel_idx, const char* channel_name, const char* text,
                    uint32_t ts, uint8_t path_len, float snr) override;
  void onAckDelivered(uint32_t ack, const ContactInfo* contact, uint32_t trip_ms) override;
  void onAdvertSeen(const ContactInfo& contact, bool is_new, uint8_t path_len) override;
  void onRawRx(float snr, float rssi, int len) override;
  void onTraceResult(uint32_t tag, uint8_t path_len, const uint8_t* path_hashes,
                     const uint8_t* path_snrs, uint8_t snr_count, float final_snr) override;
  void onSendTimeout() override;
  void logF(const char* fmt, ...) override;

  // ---- services for screens ----
  GFXcanvas16& cv() { return hw.cv(); }
  DeckHW hw;
  MessageStore store;
  SDMaps sdmaps;               // classic high-detail coastline packs (.mdm)
  NewMaps newmaps;             // NewMaps multi-layer OSM packs (.mdv)
  void reloadSDMaps();         // rescan the card (Settings action)
  MyMesh* mesh = nullptr;
  SensorManager* sensors = nullptr;
  NodePrefs* prefs = nullptr;
  DeckSettings set;

  void go(ScreenId id);
  void back();
  void goHome();
  void toast(const char* msg, uint16_t color = C_ACCENT);
  void requestDraw() { _dirty = true; }
  void saveSettings();
  void applySettings();

  // Room / repeater login credentials (SPIFFS) + auto-login
  static constexpr int ROOM_CRED_MAX = 16;
  static constexpr uint8_t AUTO_LOGIN_TRIES_DEFAULT = 5;
  static constexpr uint8_t AUTO_LOGIN_TRIES_MAX = 20;
  uint8_t autoLoginMaxTries() const;  // from DeckSettings, clamped
  struct RoomCred {
    uint8_t pub_prefix[6];
    char    password[16];   // MeshCore max 15 + NUL
    uint8_t auto_login;     // 1 = re-login after reboot (rooms only; ignored for repeaters)
    uint8_t type;           // ADV_TYPE_ROOM / REPEATER
    char    name[24];
  };
  void loadRoomCreds();
  void saveRoomCreds();
  const RoomCred* findRoomCred(const uint8_t* prefix6) const;
  RoomCred* findRoomCredMut(const uint8_t* prefix6);
  void saveRoomCred(const ContactInfo& c, const char* password, bool auto_login);
  void forgetRoomCred(const uint8_t* prefix6);
  bool setRoomAutoLogin(const uint8_t* prefix6, bool on);
  // from_auto=false (manual): may preempt a different peer's in-flight login.
  // from_auto=true: never interrupts a manual login; skips if any login pending.
  bool beginRoomLogin(const ContactInfo& c, const char* password, bool auto_login_if_ok,
                      bool force = false, bool from_auto = false);
  void tryAutoLoginRooms();   // call after boot / when repeaters list opens
  void onLoginResult(const ContactInfo& from, bool ok) override;
  bool roomSessionOk(const uint8_t* prefix6) const;  // logged in this boot
  // After login, room servers push backlog stop-and-wait. Block TX until quiet.
  bool isRoomSyncing(const uint8_t* prefix6) const;
  // Remaining ms until send allowed (0 = ready). Also true for non-rooms.
  uint32_t roomSyncRemainingMs(const uint8_t* prefix6) const;
  bool allowSendToContact(const ContactInfo& c, bool toast_if_blocked = true);
  // Re-login and re-request backlog (full_history zeros sync_since)
  bool resyncRoom(const ContactInfo& c, bool full_history = true);
  int  roomCredCount() const { return _room_cred_n; }
  const RoomCred* roomCredAt(int i) const {
    return (i >= 0 && i < _room_cred_n) ? &_room_creds[i] : nullptr;
  }

  // discovery / diagnostics / SOS
  void discover();                     // send a flood advert + jump to Heard
  uint32_t rxCount() const { return _rx_count; }
  void applyPreset(float freq, float bw, uint8_t sf, uint8_t cr);  // onboarding radio preset
  void finishOnboarding();             // mark configured + go home
  void toggleSOS();                    // start/stop the SOS beacon
  bool sosActive() const { return _sos_active; }
  void sendSOSNow();

  // WiFi + connectivity (T-Deck Plus: built-in GPS, WiFi on the ESP32-S3)
  void loadWifi();
  void saveWifi();
  void wifiConnect();                  // connect using stored SSID/pass
  void wifiOff();
  int  wifiState() const;              // 0 off, 1 connecting, 2 connected
  char* wifiSsid() { return _wifi_ssid; }
  char* wifiPass() { return _wifi_pass; }
  bool  gpsFix() const { return sensors && (sensors->node_lat != 0 || sensors->node_lon != 0); }
  const char* downloadMapPack(const char* name);   // fetch a .mdm over WiFi to SD; nullptr = ok
  const char* downloadMapIndex(char* buf, size_t sz, int& outlen);  // fetch maps/index.txt over WiFi
  const char* prepareSD();                          // create /meshdeck-maps on the SD card

  // chat actions
  bool sendDM(const uint8_t* pub_prefix, const char* text);       // to contact by 6-byte prefix
  bool sendChannel(uint8_t channel_idx, const char* text);
  void openThread(int thread_idx);                                // jumps to chat screen

  // channels (mesh slot indices; empty slots have blank name + zero secret)
  int  channelCount();                                            // occupied slots only
  int  channelSlotAt(int list_idx);                               // list row -> mesh index (-1)
  bool channelNameAt(int mesh_idx, char* out, size_t sz);
  bool channelKeyBase64(int mesh_idx, char* out, size_t sz);      // 16- or 32-byte secret as b64
  bool channelKeyHex(int mesh_idx, char* out, size_t sz);         // secret as lowercase hex
  // Official: meshcore://channel/add?name=...&secret=<hex>
  bool channelShareUrl(int mesh_idx, char* out, size_t sz);
  bool channelIsPublic(int mesh_idx);                             // default MeshCore Public PSK
  int  findChannelBySecret(const uint8_t* secret, int seclen);    // mesh idx or -1
  void openChannel(int channel_idx);                              // open that channel's chat thread
  // psk_base64 empty: hashtag (#name -> sha256) or random private key.
  // Returns mesh slot index, or -1 on failure.
  int addChannelNamed(const char* name, const char* psk_base64);
  // Join from official share (raw secret bytes). -1 fail; existing slot if already joined.
  int addChannelFromSecret(const char* name, const uint8_t* secret, int seclen);
  bool renameChannel(int mesh_idx, const char* name);
  bool rekeyChannel(int mesh_idx, const char* psk_base64);        // empty = new random key
  bool removeChannel(int mesh_idx);                               // clear slot (companion-compatible)
  void openQR(const char* url);
  int  pendingThread() const { return _pending_thread; }
  void clearPendingThread() { _pending_thread = -1; }
  const char* qrUrl() const { return _qr_url; }

  // terminal
  void termLog(uint16_t color, const char* fmt, ...);
  TermLine* termLine(int i);           // 0 oldest
  int termCount() const { return _term_count; }
  void termClear() { _term_count = 0; _term_head = 0; }

  // last heard
  int heardCount() const { return _heard_count; }
  const HeardEntry* heardAt(int i) const;   // 0 = newest

  // noise
  const int8_t* noiseRing(int& head) const { head = _noise_head; return _noise; }
  int lastNoise() const { return _last_noise; }
  float lastRxRssi() const { return _last_rx_rssi; }
  float lastRxSnr() const { return _last_rx_snr; }
  uint32_t lastRxMillis() const { return _last_rx_millis; }

  // trace / voice (contact-oriented actions)
  bool startTrace(const ContactInfo& target);
  bool startVoiceCall(const ContactInfo& to);   // Contacts -> Call... (beta PTT)
  TraceResult trace;

  // repeater console (CLI responses from repeaters)
  // Local/system line for the open repeater console (from usually ">")
  void repLog(const char* from, const char* text);
  // Remote room/repeater line - only appears if console is open for that peer
  void repLogFrom(const ContactInfo& from, const char* text);

  // status helpers
  int batteryPercent() const;
  int meshBars() const;                // 0..4 based on recent heard + snr
  bool ownPos(double& lat, double& lon) const;
  uint32_t epochNow() const;
  uint32_t localEpoch() const;                    // epochNow() + timezone offset
  void fmtClock(char* out, size_t sz) const;      // "14:05" (local)
  void fmtAgo(char* out, size_t sz, uint32_t epoch_then) const;
  // Contact out_path as "1a > 14 > 2b" (or "flood" / "direct")
  void fmtContactPath(char* out, size_t sz, const ContactInfo& ct) const;
  void drawStatusBar(const char* title);

  // number/symbol entry layer (works regardless of the keyboard chip's alt key)
  void toggleSym() { _sym_shift = !_sym_shift; _dirty = true; }
  bool symShift() const { return _sym_shift; }

  // remote screen: mirror the display to a browser over WiFi and accept input
  void startRemoteScreen();
  void stopRemoteScreen();
  bool remoteScreenOn() const { return _remote_on; }
  const char* remoteScreenURL();
  // input injected by the web client (consumed in dispatchInput)
  volatile uint8_t _inj_key = 0;
  uint8_t _dim_level = 0;          // last backlight level set by always-on-clock dimming (#1)
  volatile int     _inj_nav = 0;
  volatile bool    _inj_tap = false;
  volatile int16_t _inj_x = 0, _inj_y = 0;

  ContactInfo* contactByPrefix(const uint8_t* prefix6);

  // used by LastHeardScreen "Save contact"
  ContactInfo* findRecentContact(const uint8_t* prefix6);
  void rememberRecentContact(const ContactInfo& c);


private:
  void dispatchInput();
  void drawAll();
  void notifyPopupDraw();
  void checkDim();

  Screen* _screens[SCR_COUNT];
  ScreenId _cur = SCR_HOME;
  ScreenId _stack[8];
  int _stack_len = 0;
  bool _dirty = true;
  bool _booted = false;
  uint32_t _last_tick = 0;
  uint32_t _last_noise_sample = 0;
  uint32_t _last_settings_refresh = 0;
  uint32_t _last_select_ms = 0;     // for double-click-to-back detection

  // toast/notify popup
  char _toast[64];
  uint16_t _toast_color = C_ACCENT;
  uint32_t _toast_until = 0;

  // room/repeater saved logins
  RoomCred _room_creds[ROOM_CRED_MAX];
  int      _room_cred_n = 0;
  // Per-cred session flag: already successfully logged in this boot (not persisted)
  uint8_t  _room_session_ok[ROOM_CRED_MAX] = {0};
  // Per-cred auto-login TX attempts this boot (not persisted)
  uint8_t  _room_auto_tries[ROOM_CRED_MAX] = {0};
  uint8_t  _auto_login_wait_rounds = 0;  // contact-not-found reschedule budget
  uint8_t  _login_pending_prefix[6] = {0};
  char     _login_pending_pwd[16] = {0};
  bool     _login_pending_auto = true;   // save auto_login flag if this attempt succeeds
  bool     _login_pending_from_auto = false;  // true if boot auto-login started this attempt
  bool     _login_pending_valid = false;
  uint32_t _login_pending_ms = 0;  // millis when pending started (timeout stuck logins)
  uint32_t _auto_login_at = 0;   // millis when to run next auto-login (0 = idle)
  static constexpr uint32_t LOGIN_PENDING_TIMEOUT_MS = 20000;

  // Room backlog sync guard (half-duplex: TX mid-push drops server ACKs)
  static constexpr uint32_t ROOM_SYNC_MIN_MS   = 3000;   // min block after login
  static constexpr uint32_t ROOM_SYNC_QUIET_MS = 5000;   // no posts for this long
  static constexpr uint32_t ROOM_SYNC_MAX_MS   = 60000;  // hard cap
  static constexpr int      ROOM_SYNC_SLOTS    = 4;
  struct RoomSync {
    uint8_t  prefix[6];
    uint32_t login_ms;
    uint32_t last_rx_ms;
    bool     active;
  };
  RoomSync _room_sync[ROOM_SYNC_SLOTS] = {};

  int  roomCredIndex(const uint8_t* prefix6) const;
  void markRoomSessionOk(const uint8_t* prefix6);
  bool roomNeedsAutoLogin(int idx) const;
  void clearLoginPending(const char* why);
  void beginRoomSync(const uint8_t* prefix6);
  void noteRoomSyncRx(const uint8_t* prefix6);
  void endRoomSync(const uint8_t* prefix6, const char* why);
  int  roomSyncIndex(const uint8_t* prefix6) const;

  // pending nav
  int _pending_thread = -1;
  char _qr_url[128];

  // terminal ring
  TermLine* _term = nullptr;
  int _term_count = 0, _term_head = 0;

  // heard ring
  HeardEntry _heard[HEARD_MAX];
  int _heard_count = 0, _heard_head = 0;

  // recent full contacts seen via advert (so Last Heard -> Save works with auto-add off)
  static const int RECENT_CONTACTS = 16;
  ContactInfo _recent_ct[RECENT_CONTACTS];
  int _recent_ct_count = 0;
  int _recent_ct_head = 0;


  // noise ring
  int8_t _noise[NOISE_SAMPLES];
  int _noise_head = 0;
  int _last_noise = -120;
  float _last_rx_rssi = -130, _last_rx_snr = 0;
  uint32_t _last_rx_millis = 0;
  uint32_t _rx_count = 0;            // total raw packets received (diagnostics)

  // auto-advert + SOS beacon
  uint32_t _last_auto_adv = 0;
  double   _adv_last_lat = 0, _adv_last_lon = 0;   // last move-advert position (#10)
  uint32_t _adv_move_ms = 0;                       // time of last move-triggered advert (#10)
  uint8_t  _cpu_mhz = 240;          // current CPU clock, for the power saver (#14)
  bool _sos_active = false;
  uint32_t _sos_last = 0;

  // WiFi
  char _wifi_ssid[33] = "";
  char _wifi_pass[65] = "";
  bool _wifi_want = false;

  // number/symbol layer state (see dispatchInput)
  bool _sym_shift = false;

  // NTP clock sync over WiFi
  bool _ntp_started = false;
  bool _ntp_done = false;
  uint32_t _ntp_last_try = 0;

  // remote screen web server
  WebServer* _web = nullptr;
  bool _remote_on = false;
  char _remote_url[40] = "";

  // serial terminal input
  char _ser_line[96];
  int _ser_len = 0;
};

// text helpers (implemented in UITask.cpp, used by screens)
// Adafruit GFX default font is 7-bit ASCII only - never pass UTF-8 / fancy punctuation
// to print()/printf or you get garbage glyphs.
int drawRichText(GFXcanvas16& cv, int x, int y, int max_w, const char* text,
                 uint16_t color, int text_size);   // returns height used; handles wrap + emoji
int measureRichTextHeight(GFXcanvas16& cv, int max_w, const char* text, int text_size);
// Copy src -> dst, keep printable ASCII (0x20-0x7E), replace others with '?', then ellipsize.
void ellipsize(char* dst, size_t dst_sz, const char* src);
// In-place: keep only printable ASCII; multi-byte UTF-8 -> single '?'.
void sanitizeAscii(char* s);
