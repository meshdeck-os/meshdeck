#include "MessageStore.h"
#include "Theme.h"
#include <helpers/TxtDataHelpers.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>

#define MSG_FILE "/meshdeck_msgs.bin"
#define MSG_FILE_MAGIC 0x4D444B31   // "MDK1"
#define SAVE_DEBOUNCE_MS 8000
#define PERSIST_PER_THREAD 16       // keep the newest N per thread on flash

uint16_t nameColor(const char* name) {
  uint32_t h = 5381;
  while (*name) h = h * 33 + (uint8_t)(*name++);
  return NAME_COLORS[h & 7];
}

DeckMsg* MessageStore::allocRing() {
  size_t sz = sizeof(DeckMsg) * MD_THREAD_MSGS;
  DeckMsg* m = (DeckMsg*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!m) m = (DeckMsg*)malloc(sz);
  if (m) memset(m, 0, sz);
  return m;
}

void MessageStore::begin() {
  memset(_threads, 0, sizeof(_threads));
  _num = 0;
  load();
}

DeckThread* MessageStore::forChannel(uint8_t channel_idx, const char* name) {
  for (int i = 0; i < _num; i++) {
    if (_threads[i].kind == TK_CHANNEL && _threads[i].channel_idx == channel_idx) {
      if (name && name[0]) StrHelper::strncpy(_threads[i].title, name, sizeof(_threads[i].title));
      return &_threads[i];
    }
  }
  if (_num >= MD_MAX_THREADS) return nullptr;
  DeckThread* t = &_threads[_num];
  memset(t, 0, sizeof(*t));
  t->kind = TK_CHANNEL;
  t->channel_idx = channel_idx;
  StrHelper::strncpy(t->title, (name && name[0]) ? name : "channel", sizeof(t->title));
  t->msgs = allocRing();
  if (!t->msgs) return nullptr;
  _num++;
  return t;
}

DeckThread* MessageStore::forContact(const uint8_t* pub_key, const char* name) {
  for (int i = 0; i < _num; i++) {
    if (_threads[i].kind == TK_CONTACT && memcmp(_threads[i].pub_prefix, pub_key, 6) == 0) {
      if (name && name[0]) StrHelper::strncpy(_threads[i].title, name, sizeof(_threads[i].title));
      return &_threads[i];
    }
  }
  if (_num >= MD_MAX_THREADS) return nullptr;
  DeckThread* t = &_threads[_num];
  memset(t, 0, sizeof(*t));
  t->kind = TK_CONTACT;
  memcpy(t->pub_prefix, pub_key, 6);
  StrHelper::strncpy(t->title, (name && name[0]) ? name : "???", sizeof(t->title));
  t->msgs = allocRing();
  if (!t->msgs) return nullptr;
  _num++;
  return t;
}

DeckMsg* MessageStore::addMsg(DeckThread* t, const char* sender, const char* text,
                              uint32_t ts, uint8_t flags, int8_t snr4, uint8_t hops, uint32_t ack) {
  if (!t || !t->msgs) return nullptr;
  DeckMsg* m = &t->msgs[t->head];
  memset(m, 0, sizeof(*m));
  m->ts = ts;
  m->ack = ack;
  m->snr4 = snr4;
  m->hops = hops;
  m->flags = flags;
  if (sender) StrHelper::strncpy(m->sender, sender, sizeof(m->sender));
  StrHelper::strncpy(m->text, text, sizeof(m->text));
  t->head = (t->head + 1) % MD_THREAD_MSGS;
  if (t->count < MD_THREAD_MSGS) t->count++;
  t->last_ts = ts;
  if (!(flags & MF_OUT)) t->unread++;
  _dirty_at = millis();
  return m;
}

DeckMsg* MessageStore::msgAt(DeckThread* t, int i) {
  if (!t || !t->msgs || i < 0 || i >= t->count) return nullptr;
  int start = (t->head - t->count + MD_THREAD_MSGS * 2) % MD_THREAD_MSGS;
  return &t->msgs[(start + i) % MD_THREAD_MSGS];
}

bool MessageStore::markDelivered(uint32_t ack) {
  if (ack == 0) return false;
  for (int i = 0; i < _num; i++) {
    DeckThread* t = &_threads[i];
    for (int j = 0; j < t->count; j++) {
      DeckMsg* m = msgAt(t, j);
      if (m && (m->flags & MF_OUT) && m->ack == ack) {
        m->flags |= MF_DELIVERED;
        m->flags &= ~MF_FAILED;
        _dirty_at = millis();
        return true;
      }
    }
  }
  return false;
}

void MessageStore::markTimedOut() {
  // newest outgoing message that is neither delivered nor failed
  DeckMsg* best = nullptr;
  for (int i = 0; i < _num; i++) {
    DeckThread* t = &_threads[i];
    for (int j = 0; j < t->count; j++) {
      DeckMsg* m = msgAt(t, j);
      if (m && (m->flags & MF_OUT) && !(m->flags & (MF_DELIVERED | MF_FAILED)) && m->ack != 0) {
        if (!best || m->ts >= best->ts) best = m;
      }
    }
  }
  if (best) { best->flags |= MF_FAILED; _dirty_at = millis(); }
}

void MessageStore::markRead(DeckThread* t) {
  if (t && t->unread) { t->unread = 0; }
}

int MessageStore::totalUnread() const {
  int n = 0;
  for (int i = 0; i < _num; i++) n += _threads[i].unread;
  return n;
}

void MessageStore::sortByRecent(int* order) const {
  for (int i = 0; i < _num; i++) order[i] = i;
  for (int i = 1; i < _num; i++) {       // insertion sort by last_ts desc
    int v = order[i];
    int j = i - 1;
    while (j >= 0 && _threads[order[j]].last_ts < _threads[v].last_ts) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = v;
  }
}

void MessageStore::save() { _dirty_at = millis(); }

void MessageStore::loop() {
  if (_dirty_at && millis() - _dirty_at > SAVE_DEBOUNCE_MS) {
    _dirty_at = 0;
    persistNow();
  }
}

void MessageStore::persistNow() {
  File f = SPIFFS.open(MSG_FILE, "w");
  if (!f) return;
  uint32_t magic = MSG_FILE_MAGIC;
  uint8_t num = _num;
  f.write((uint8_t*)&magic, 4);
  f.write(&num, 1);
  for (int i = 0; i < _num; i++) {
    DeckThread* t = &_threads[i];
    f.write(&t->kind, 1);
    f.write(&t->channel_idx, 1);
    f.write(t->pub_prefix, 6);
    f.write((uint8_t*)t->title, sizeof(t->title));
    f.write((uint8_t*)&t->last_ts, 4);
    uint8_t n = t->count > PERSIST_PER_THREAD ? PERSIST_PER_THREAD : t->count;
    f.write(&n, 1);
    for (int j = t->count - n; j < t->count; j++) {
      DeckMsg* m = msgAt(t, j);
      f.write((uint8_t*)m, sizeof(DeckMsg));
    }
  }
  f.close();
}

void MessageStore::load() {
  File f = SPIFFS.open(MSG_FILE, "r");
  if (!f) return;
  uint32_t magic = 0;
  uint8_t num = 0;
  if (f.read((uint8_t*)&magic, 4) != 4 || magic != MSG_FILE_MAGIC) { f.close(); return; }
  f.read(&num, 1);
  if (num > MD_MAX_THREADS) num = MD_MAX_THREADS;
  for (int i = 0; i < num; i++) {
    DeckThread* t = &_threads[_num];
    memset(t, 0, sizeof(*t));
    f.read(&t->kind, 1);
    f.read(&t->channel_idx, 1);
    f.read(t->pub_prefix, 6);
    f.read((uint8_t*)t->title, sizeof(t->title));
    f.read((uint8_t*)&t->last_ts, 4);
    uint8_t n = 0;
    f.read(&n, 1);
    if (n > PERSIST_PER_THREAD) n = PERSIST_PER_THREAD;
    t->msgs = allocRing();
    if (!t->msgs) break;
    for (int j = 0; j < n; j++) {
      f.read((uint8_t*)&t->msgs[j], sizeof(DeckMsg));
    }
    t->count = n;
    t->head = n % MD_THREAD_MSGS;
    t->unread = 0;
    _num++;
  }
  f.close();
}
