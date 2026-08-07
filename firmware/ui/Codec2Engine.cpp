#include "Codec2Engine.h"

#ifdef MESHDECK_BETA
#include <string.h>

bool Codec2Engine::begin() {
  if (_c2) return true;

  // Force codec2_create allocations into PSRAM
  heap_caps_malloc_extmem_enable(0);
  Serial.printf("[c2] attempting create with MODE=%d\n", MODE);
#if defined(CODEC2_MODE_1200_EN)
  Serial.println("[c2] CODEC2_MODE_1200_EN is defined");
#else
  Serial.println("[c2] CODEC2_MODE_1200_EN is NOT defined");
#endif
  _c2 = codec2_create(MODE);

  // Restore normal threshold
  heap_caps_malloc_extmem_enable(16 * 1024);

  if (!_c2) {
    Serial.println("[c2] codec2_create failed");
    return false;
  }

  _spf  = codec2_samples_per_frame(_c2);
  _bpf  = codec2_bytes_per_frame(_c2);
  _bits = codec2_bits_per_frame(_c2);

  Serial.printf("[c2] mode=%d  spf=%d  bpf=%d  bits=%d\n", MODE, _spf, _bpf, _bits);

  // Mode 1600: spf must be 320, bpf is normally 8
  if (_spf != PCM8_SAMPLES || _bpf <= 0 || _bpf > 8) {
    Serial.printf("[c2] size check failed (spf=%d bpf=%d)\n", _spf, _bpf);
    end();
    return false;
  }

  // Allocate working buffers from PSRAM
  _pkt   = (uint8_t*) heap_caps_malloc(MAX_FRAMES * 8,               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  _tmp8  = (int16_t*)heap_caps_malloc(PCM8_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  _tmp16 = (int16_t*)heap_caps_malloc(PCM16_SAMPLES * sizeof(int16_t),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!_pkt || !_tmp8 || !_tmp16) {
    Serial.println("[c2] PSRAM buffer alloc failed");
    end();
    return false;
  }

  clearPacket();
  Serial.println("[c2] begin OK");
  return true;
}

void Codec2Engine::end() {
  if (_c2) {
    codec2_destroy(_c2);
    _c2 = nullptr;
  }
  if (_pkt)   { heap_caps_free(_pkt);   _pkt = nullptr; }
  if (_tmp8)  { heap_caps_free(_tmp8);  _tmp8 = nullptr; }
  if (_tmp16) { heap_caps_free(_tmp16); _tmp16 = nullptr; }
  clearPacket();
}

void Codec2Engine::downsample16to8(const int16_t* in16, int n16, int16_t* out8) {
  // 2-tap average is fine; mild low-pass before Codec2 8 kHz Nyquist
  int n8 = n16 / 2;
  for (int i = 0; i < n8; ++i) {
    int32_t s = (int32_t)in16[i * 2] + (int32_t)in16[i * 2 + 1];
    out8[i] = (int16_t)(s >> 1);
  }
}

void Codec2Engine::upsample8to16(const int16_t* in8, int n8, int16_t* out16) {
  // Linear interpolate — fewer staircase artifacts than zero-order hold
  for (int i = 0; i < n8 - 1; ++i) {
    int32_t a = in8[i];
    int32_t b = in8[i + 1];
    out16[i * 2]     = (int16_t)a;
    out16[i * 2 + 1] = (int16_t)((a + b) >> 1);
  }
  out16[(n8 - 1) * 2] = out16[(n8 - 1) * 2 + 1] = in8[n8 - 1];
}

bool Codec2Engine::encode16k(const int16_t* pcm16, int samples16) {
  if (!_c2 || !pcm16 || samples16 < PCM16_SAMPLES || !_tmp8 || !_pkt) {
    Serial.printf("[c2] encode16k bad args  c2=%p pcm=%p tmp8=%p pkt=%p\n",
                  _c2, pcm16, _tmp8, _pkt);
    return false;
  }

  int frames = samples16 / PCM16_SAMPLES;
  for (int f = 0; f < frames; ++f) {
    if (_frame_count >= FRAMES_PER_PKT) return true;

    downsample16to8(pcm16 + f * PCM16_SAMPLES, PCM16_SAMPLES, _tmp8);

    // Extra safety – these should never be null after a successful begin()
    if (!_c2 || !_tmp8 || !_pkt) {
      Serial.println("[c2] null pointer just before codec2_encode");
      return false;
    }

    codec2_encode(_c2, _pkt + _pkt_len, _tmp8);
    _pkt_len += _bpf;
    _frame_count++;
  }
  return _frame_count >= FRAMES_PER_PKT;
}

bool Codec2Engine::flush() {
  return _frame_count > 0;
}

int Codec2Engine::decodeFrame(const uint8_t* bytes, int16_t* pcm16_out) {
  if (!_c2 || !bytes || !pcm16_out || !_tmp8) return 0;
  codec2_decode(_c2, _tmp8, bytes);
  upsample8to16(_tmp8, _spf, pcm16_out);
  return PCM16_SAMPLES;
}

int Codec2Engine::decodePacket(const uint8_t* packet, size_t packet_len,
                               int16_t* pcm16_out, size_t max_samples) {
  if (!_c2 || !packet || !pcm16_out || _bpf == 0) return 0;
  int frames = packet_len / _bpf;
  int written = 0;
  for (int i = 0; i < frames; ++i) {
    if ((size_t)(written + PCM16_SAMPLES) > max_samples) break;
    decodeFrame(packet + i * _bpf, pcm16_out + written);
    written += PCM16_SAMPLES;
  }
  return written;
}

#endif // MESHDECK_BETA