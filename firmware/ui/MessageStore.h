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

// DeckMsg.flags bits
#define MF_OUT        0x01
#define MF_DELIVERED  0x02
#define MF_FAILED     0x04

struct DeckMsg {
  uint32_t ts;                    // epoch seconds
  uint32_t ack;                   // expected ack (outgoing), 0 otherwise
  int8_t   snr4;                  // SNR * 4 (incoming)
  uint8_t  hops;                  // 0xFF = direct
  uint8_t  flags;
  char sender[MD_SENDER_LEN];     // display name (channel messages)
  char text[MD_TEXT_LEN];
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
                  uint32_t ts, uint8_t flags, int8_t snr4, uint8_t hops, uint32_t ack);
  DeckMsg* msgAt(DeckThread* t, int i);             // i: 0 = oldest .. count-1 = newest
  bool markDelivered(uint32_t ack);                 // returns true if an outgoing msg matched
  void markTimedOut();                              // newest un-acked outgoing -> failed
  void markRead(DeckThread* t);
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
