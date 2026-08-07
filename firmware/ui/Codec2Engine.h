#pragma once
/*
 * Codec2 engine — only available in MESHDECK_BETA builds (codec2 sources + flags).
 * Stable builds must not include codec2 headers or link this code.
 */
#include <Arduino.h>

#ifndef MESHDECK_BETA
// Stub so accidental includes still compile; never used on stable.
class Codec2Engine {
public:
  bool begin() { return false; }
  void end() {}
  bool ready() const { return false; }
};
#else

#include "codec2.h"
#include <esp_heap_caps.h>

class Codec2Engine {
public:
  static constexpr int MODE = CODEC2_MODE_1200;
  static constexpr int FRAME_MS       = 40;
  // Pack frames per LoRa REQ. Mode 1200 = 6 B/frame.
  // MeshCore sendRequest: data_len <= MAX_PACKET_PAYLOAD-16 (184-16=168).
  // Wire: [type][ver][flags][seq][codec…] → data_len = 4 + codec_len
  // → codec_len <= 164 → max 27 frames. Use 24 for headroom (~0.96 s / packet).
  static constexpr int FRAMES_PER_PKT = 24;
  static constexpr int MAX_FRAMES     = 64;
  static constexpr int PCM8_SAMPLES   = 320;
  static constexpr int PCM16_SAMPLES  = 640;

  Codec2Engine() = default;
  ~Codec2Engine() { end(); }

  bool begin();
  void end();
  bool ready() const { return _c2 != nullptr; }

  // Encode path (16 kHz mic → packet)
  bool encode16k(const int16_t* pcm16, int samples16);
  bool flush();
  const uint8_t* packetData() const { return _pkt; }
  size_t         packetLen()  const { return _pkt_len; }
  void           clearPacket() { _frame_count = 0; _pkt_len = 0; }

  // Decode path (packet → 16 kHz speaker)
  int decodeFrame(const uint8_t* bytes, int16_t* pcm16_out);
  int decodePacket(const uint8_t* packet, size_t packet_len,
                   int16_t* pcm16_out, size_t max_samples);

  int bytesPerFrame() const { return _bpf; }

private:
  struct CODEC2* _c2 = nullptr;
  int _spf = 0, _bpf = 0, _bits = 0;

  // These used to be fixed-size members (internal SRAM).
  // Now allocated from PSRAM in begin().
  uint8_t*  _pkt   = nullptr;
  int16_t*  _tmp8  = nullptr;
  int16_t*  _tmp16 = nullptr;

  int      _frame_count = 0;
  size_t   _pkt_len = 0;

  void downsample16to8(const int16_t* in16, int n16, int16_t* out8);
  void upsample8to16(const int16_t* in8, int n8, int16_t* out16);
};

#endif // MESHDECK_BETA