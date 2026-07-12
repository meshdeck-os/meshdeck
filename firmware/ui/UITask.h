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
#include "Theme.h"
#include <helpers/SensorManager.h>

class MyMesh;
class UITask;

enum ScreenId : uint8_t {
  SCR_HOME = 0, SCR_CHAT, SCR_CONTACTS, SCR_MAP, SCR_LASTHEARD, SCR_REPEATERS,
  SCR_TRACE, SCR_NOISE, SCR_TERMINAL, SCR_SETTINGS, SCR_QR,
  SCR_ONBOARD, SCR_DIAG, SCR_SOS, SCR_COUNT
};

class Screen {
public:
  Screen(UITask& u) : ui(u) {}
  virtual void enter() {}
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
  uint8_t reserved[4];
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
  void onContactMsg(const ContactInfo& from, const char* text, uint32_t sender_ts,
                    uint8_t path_len, float snr) override;
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
  SDMaps sdmaps;               // high-detail map packs from SD card
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

  // discovery / diagnostics / SOS
  void discover();                     // send a flood advert + jump to Heard
  uint32_t rxCount() const { return _rx_count; }
  void applyPreset(float freq, float bw, uint8_t sf, uint8_t cr);  // onboarding radio preset
  void finishOnboarding();             // mark configured + go home
  void toggleSOS();                    // start/stop the SOS beacon
  bool sosActive() const { return _sos_active; }
  void sendSOSNow();

  // chat actions
  bool sendDM(const uint8_t* pub_prefix, const char* text);       // to contact by 6-byte prefix
  bool sendChannel(uint8_t channel_idx, const char* text);
  void openThread(int thread_idx);                                // jumps to chat screen
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

  // trace
  bool startTrace(const ContactInfo& target);
  TraceResult trace;

  // repeater console (CLI responses from repeaters)
  void repLog(const char* from, const char* text);

  // status helpers
  int batteryPercent() const;
  int meshBars() const;                // 0..4 based on recent heard + snr
  bool ownPos(double& lat, double& lon) const;
  uint32_t epochNow() const;
  void fmtClock(char* out, size_t sz) const;      // "14:05"
  void fmtAgo(char* out, size_t sz, uint32_t epoch_then) const;
  void drawStatusBar(const char* title);

  ContactInfo* contactByPrefix(const uint8_t* prefix6);

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

  // pending nav
  int _pending_thread = -1;
  char _qr_url[128];

  // terminal ring
  TermLine* _term = nullptr;
  int _term_count = 0, _term_head = 0;

  // heard ring
  HeardEntry _heard[HEARD_MAX];
  int _heard_count = 0, _heard_head = 0;

  // noise ring
  int8_t _noise[NOISE_SAMPLES];
  int _noise_head = 0;
  int _last_noise = -120;
  float _last_rx_rssi = -130, _last_rx_snr = 0;
  uint32_t _last_rx_millis = 0;
  uint32_t _rx_count = 0;            // total raw packets received (diagnostics)

  // auto-advert + SOS beacon
  uint32_t _last_auto_adv = 0;
  bool _sos_active = false;
  uint32_t _sos_last = 0;

  // serial terminal input
  char _ser_line[96];
  int _ser_len = 0;
};

// text helpers (implemented in UITask.cpp, used by screens)
int drawRichText(GFXcanvas16& cv, int x, int y, int max_w, const char* text,
                 uint16_t color, int text_size);   // returns height used; handles wrap + emoji
int measureRichTextHeight(GFXcanvas16& cv, int max_w, const char* text, int text_size);
void ellipsize(char* dst, size_t dst_sz, const char* src);
