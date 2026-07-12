#pragma once

#include <MeshCore.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/ContactInfo.h>
#include <Arduino.h>

#include "NodePrefs.h"

enum class UIEventType {
    none,
    contactMessage,
    channelMessage,
    roomMessage,
    newContactMessage,
    ack
};

/*
 * MeshDeck: extended UI hook interface.
 * The base methods match the stock companion_radio AbstractUITask so MyMesh.cpp
 * only needs light touch-ups; the on*() hooks below deliver the rich event data
 * that the standalone MeshDeck UI needs (full message routing, signal info,
 * trace results, raw RX activity for the noise/terminal screens, etc).
 */
class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  BaseSerialInterface* _serial;
  bool _connected;

  AbstractUITask(mesh::MainBoard* board, BaseSerialInterface* serial) : _board(board), _serial(serial) {
    _connected = false;
  }

public:
  void setHasConnection(bool connected) { _connected = connected; }
  bool hasConnection() const { return _connected; }
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  bool isSerialEnabled() const { return _serial->isEnabled(); }
  void enableSerial() { _serial->enable(); }
  void disableSerial() { _serial->disable(); }
  virtual void msgRead(int msgcount) = 0;
  virtual void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) = 0;
  virtual void notify(UIEventType t = UIEventType::none) = 0;
  virtual void loop() = 0;

  // ---- MeshDeck rich event hooks (default no-op) ----

  // direct message (txt_type TXT_TYPE_PLAIN / _SIGNED_PLAIN) from a known contact
  virtual void onContactMsg(const ContactInfo& from, const char* text, uint32_t sender_ts,
                            uint8_t path_len, float snr) {}
  // CLI response data from a repeater/room we are logged into
  virtual void onCliResponse(const ContactInfo& from, const char* text) {}
  // group channel message; text is "SenderName: message" per MeshCore convention
  virtual void onChannelMsg(uint8_t channel_idx, const char* channel_name, const char* text,
                            uint32_t ts, uint8_t path_len, float snr) {}
  // delivery report: an expected ACK arrived (contact may be NULL)
  virtual void onAckDelivered(uint32_t ack, const ContactInfo* contact, uint32_t trip_ms) {}
  // an advert was received/updated (drives last-heard list + map)
  virtual void onAdvertSeen(const ContactInfo& contact, bool is_new, uint8_t path_len) {}
  // every raw LoRa packet received (drives activity LED/noise screen/terminal)
  virtual void onRawRx(float snr, float rssi, int len) {}
  // trace route result
  virtual void onTraceResult(uint32_t tag, uint8_t path_len, const uint8_t* path_hashes,
                             const uint8_t* path_snrs, uint8_t snr_count, float final_snr) {}
  // a send timed out with no ACK
  virtual void onSendTimeout() {}
  // free-form log line for the terminal screen (also mirrored to USB serial)
  virtual void logF(const char* fmt, ...) {}
};
