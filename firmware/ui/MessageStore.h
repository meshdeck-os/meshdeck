#pragma once
/*
 * MeshDeck message store: per-thread ring buffers (channels + DM threads),
 * unread tracking, delivery status, persisted to SPIFFS.
 */
#include <Arduino.h>
#include <helpers/ContactInfo.h>

#define MD_MAX_THREADS   32
#define MD_THREAD_MSGS   48
#define MD_TEXT_LEN      168
#define MD_SENDER_LEN    16
// Route a received message took (Message info in chat). Kept in RAM next to
// each thread's ring, NOT saved: the saved file stores DeckMsg as raw bytes,
// so DeckMsg itself must not change. After a reboot older messages have none.
#define MD_ROUTE_BYTES   32               // 16 hops of 2-byte hashes, 32 of 1-byte
#define MD_ROUTE_REC     (2 + MD_ROUTE_BYTES)   // [0] kind, [1] raw path_len, [2..] hashes
#define ROUTE_NONE       0                      // not recorded (sent, or from before a reboot)
#define ROUTE_FLOOD      1                      // flood: [2..] lists the repeaters passed
#define ROUTE_DIRECT     2                      // sent along a known route; hops not listed

// DeckMsg.flags bits
#define MF_OUT        0x01
#define MF_DELIVERED  0x02
#define MF_FAILED     0x04
#define MF_VOICE  0x10



struct DeckMsg {
  uint32_t ts;                    // epoch seconds
  uint32_t ack;                   // expected ack (outgoing), 0 otherwise
  int8_t   snr4;                  // SNR * 4 (incoming)
  uint8_t  hops;                  // 0xFF = direct
  uint8_t  flags;
  char sender[MD_SENDER_LEN];     // display name (channel messages)
  char text[MD_TEXT_LEN];
  // Voice payload (only valid when MF_VOICE is set)
  uint16_t voice_len = 0;
  uint8_t* voice_data = nullptr;   // PSRAM, owned by the msg
};

// DeckThread.kind
#define TK_CHANNEL 0
#define TK_CONTACT 1

struct DeckThread {
  uint8_t  kind;
  uint8_t  channel_idx;           // TK_CHANNEL
  uint8_t  pub_prefix[6];         // TK_CONTACT
  char     title[28];
  uint16_t unread;
  uint32_t last_ts;
  uint16_t count;                 // number of valid msgs (<= MD_THREAD_MSGS)
  uint16_t head;                  // next write slot
  DeckMsg* msgs;                  // ring buffer, PSRAM
  uint8_t* routes;                // MD_THREAD_MSGS route records, parallel to msgs (RAM only)
};

class MessageStore {
public:
  void begin();                                     // alloc + load persisted history
  int  numThreads() const { return _num; }
  DeckThread* thread(int i) { return (i >= 0 && i < _num) ? &_threads[i] : nullptr; }

  DeckThread* forChannel(uint8_t channel_idx, const char* name);      // find-or-create
  DeckThread* forContact(const uint8_t* pub_key, const char* name);   // find-or-create
  int indexOf(DeckThread* t) const { return t ? (int)(t - _threads) : -1; }

  DeckMsg* addMsg(DeckThread* t, const char* sender, const char* text,
                uint32_t ts, uint8_t flags, int8_t snr4, uint8_t hops, uint32_t ack,
                const uint8_t* voice = nullptr, uint16_t voice_len = 0);

  DeckMsg* msgAt(DeckThread* t, int i);             // i: 0 = oldest .. count-1 = newest
  // Route record for a message (see MD_ROUTE_REC), or nullptr if none was kept.
  void setRoute(DeckThread* t, const DeckMsg* m, uint8_t kind, uint8_t path_len, const uint8_t* path);
  const uint8_t* routeOf(const DeckThread* t, const DeckMsg* m) const;
  bool markDelivered(uint32_t ack);                 // returns true if an outgoing msg matched
  void markTimedOut();                              // newest un-acked outgoing -> failed
  void markRead(DeckThread* t);
  void clearAll();                                  // wipe all message history (#11)
  int  totalUnread() const;
  void sortByRecent(int* order) const;              // fills order[numThreads()]

  void save();                                      // debounced persist
  void loop();                                      // handles pending save

private:
  DeckMsg* allocRing();
  void persistNow();
  void load();

  DeckThread _threads[MD_MAX_THREADS];
  int _num = 0;
  uint32_t _dirty_at = 0;
};

uint16_t nameColor(const char* name);   // hashed colour for usernames
