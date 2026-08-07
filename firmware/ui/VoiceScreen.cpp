#include "AllScreens.h"
#include "../MyMesh.h"
#include <Wire.h>

#ifdef MESHDECK_BETA
#include "Codec2Engine.h"
#endif

/*
 * VoiceScreen – Codec2 1200 half-duplex PTT over MeshCore
 *
 * Entry: Contacts → "Call…" → prepareOutbound + auto INVITE.
 * Inbound: global Accept/Decline overlay; Accept opens this screen.
 *
 * Half-duplex walkie model:
 *   Only one side "holds the floor" at a time. ENTER toggles PTT.
 *   While TX: mic encode + mesh; speaker/decode stopped.
 *   While RX/listen: decode + speaker; PTT may barge-in (stops RX).
 *   Leaving the screen always hangs up and tears down workers.
 *
 * Airtime: buffer ~1 s of Codec2 1200 per LoRa request (24 frames × 6 B),
 * stop-and-wait (1 inflight) so ACKs can return on half-duplex radio.
 * Size limited by MeshCore sendRequest (data_len ≤ MAX_PACKET_PAYLOAD-16).
 *
 * Frame (v2): [flags][seq][payload…]
 * Control: flags |= 0x80, low nibble = INVITE/ACCEPT/DECLINE/END/BUSY.
 */

#define VOICE_HZ          16000
#define FRAME_SAMPLES_16K 640

#ifdef MESHDECK_BETA
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#ifndef PCM16_SAMPLES
#define PCM16_SAMPLES 640
#endif

// ── Control flags (high bit = control) ────────────────────────
#define VOICE_FLAG_EOS     0x01
#define VOICE_FLAG_RETX    0x02
#define VOICE_FLAG_CTRL    0x80

#define VOICE_CTRL_INVITE  0x01
#define VOICE_CTRL_ACCEPT  0x02
#define VOICE_CTRL_DECLINE 0x03
#define VOICE_CTRL_END     0x04
#define VOICE_CTRL_BUSY    0x05

enum CallState : uint8_t {
  CALL_IDLE = 0,
  CALL_OUTGOING,   // we sent INVITE, waiting for ACCEPT
  CALL_INCOMING,   // remote INVITE received
  CALL_CONNECTED
};

static uint8_t g_es_addr = 0x40;

#define ES_MCLK        48
#define ES_SCK         47
#define ES_LRCK        21
#define ES_DIN         14
// Speaker pins: use TDECK_I2S_* from DeckHW.h (included via AllScreens/UITask)
#define BOARD_POWERON  10

static bool g_mic_started = false;

// Forward declarations
static bool mic_i2s_start();
static void mic_i2s_stop();
static bool es7210_config();
static void voiceWorkerTask(void* param);
static bool ensureVoiceWorker();
static bool ensureRxWorker();

static Codec2Engine g_c2;
static int16_t*     g_frame = nullptr;
static bool         g_ptt = false;
static uint32_t     g_ptt_start_ms = 0;
static int          g_packets_sent = 0;

// Call RX volume 1..10 (default 8). Applied on top of soft AGC at speaker.
static int          g_call_vol = 8;
// Last time we played peer audio — used for half-duplex UI / floor control
static uint32_t     g_last_rx_play_ms = 0;
static uint32_t     g_call_connected_ms = 0;
// True from PTT release until final EOS packet is queued/sent
static bool         g_tx_finishing = false;

static bool peerRecentlyTalking() {
  // Short window so UI returns to LISTEN soon after peer stops
  return g_last_rx_play_ms != 0 && (millis() - g_last_rx_play_ms) < 400;
}

static volatile CallState g_call_state = CALL_IDLE;
static volatile bool g_incoming = false;          // kept for draw compatibility
static char          g_incoming_name[32] = {0};
static float         g_incoming_snr = 0;
static uint32_t      g_call_start_ms = 0;
static constexpr uint32_t CALL_RING_TIMEOUT_MS = 30000; // 30 s

static ContactInfo  g_target{};
static bool         g_has_target = false;
static int          g_contact_idx = 0;

// Caller of an inbound ring (always prefer pubkey over name match)
static ContactInfo  g_caller{};
static bool         g_has_caller = false;

// Callee retransmits ACCEPT until caller is known connected (media/ctrl heard)
// or we give up. Without this, a lost ACCEPT leaves the caller stuck in RING.
static bool         g_accept_retx = false;
static uint32_t     g_accept_last_ms = 0;
static uint8_t      g_accept_retx_count = 0;
static constexpr uint32_t ACCEPT_RETX_MS = 2500;
static constexpr uint8_t  ACCEPT_RETX_MAX = 8;

static constexpr const char* REJECT_DM_TEXT = "Can't take a call right now";

static volatile bool g_c2_init_done = false;
static volatile bool g_c2_init_ok   = false;
static volatile bool g_c2_init_req  = false;

static StackType_t*  g_c2_stack = nullptr;
static StaticTask_t  g_c2_task_buf;
static TaskHandle_t  g_c2_task = nullptr;

static volatile bool g_voice_worker_run = false;
static StackType_t*  g_voice_stack = nullptr;  // alias of g_hd_stack while TX runs
static StaticTask_t  g_voice_task_buf;
static TaskHandle_t  g_voice_task = nullptr;

// Decode worker (codec2_decode + I2S). Large INTERNAL stack only.
// Half-duplex: not run at the same time as the TX encode worker.
static volatile bool g_dec_worker_run = false;
static StackType_t*  g_dec_stack = nullptr;  // alias of g_hd_stack while decode runs
static StaticTask_t  g_dec_task_buf;
static TaskHandle_t  g_dec_task = nullptr;

// One large INTERNAL stack shared by c2dec XOR c2work for the whole call.
// Allocated once at call connect (before small c2rx) so we grab the biggest
// free block before fragmentation from free/realloc thrash. Never free mid-call.
static StackType_t*  g_hd_stack = nullptr;
static uint32_t      g_hd_stack_bytes = 0;

// Packet produced by worker, consumed by pollPTT on loopTask
static uint8_t         g_pending_pkt[256];
static volatile int    g_pending_len = 0;
static volatile bool   g_pending_eos = false;
static volatile bool   g_flush_req  = false;

// ── Receive side ──────────────────────────────────────────────
static constexpr int RX_QUEUE_DEPTH = 8;
static constexpr int RX_PKT_MAX     = 180;

struct RxVoicePkt {
  uint8_t  data[RX_PKT_MAX];
  uint16_t len;
  bool     eos;
  bool     from_contact;
  char     name[32];
};

static RxVoicePkt   g_rx_q[RX_QUEUE_DEPTH];
static volatile int g_rx_head = 0;
static volatile int g_rx_tail = 0;
static volatile int g_rx_count = 0;

static bool         g_spk_started = false;
static TaskHandle_t g_rx_task = nullptr;
static StackType_t* g_rx_stack = nullptr;
static StaticTask_t g_rx_task_buf;

static int16_t*     g_rx_pcm = nullptr;

static bool ensureDecWorker();
static void stopDecWorker();
static void stopVoiceWorker();
static bool ensureHdStack();
static void freeHdStack();

// ============================================================
// TX retransmit + RX jitter buffer
// ============================================================

static constexpr int VOICE_ACK_SLOTS      = 16;
// SF8 / BW62.5 media ~122 B can take ~1–2 s airtime each; ACKs need a quiet RX
// window. Old 4 s timeout fired mid-PTT and RETX tags never matched ACKs.
static constexpr uint32_t VOICE_ACK_TIMEOUT_MS = 12000;
static constexpr int VOICE_MAX_RETX       = 1;   // one retx for media
// Half-duplex: only 1 unacked media in flight so the radio can hear RESP ACKs.
static constexpr int VOICE_MAX_INFLIGHT   = 1;
static constexpr int JITTER_DEPTH         = 8;
// Stop-and-wait media is 1 inflight — rarely have 2 packets buffered. Play ASAP.
static constexpr int JITTER_PLAY_THRESH   = 1;
static constexpr uint32_t JITTER_WAIT_MS  = 400;   // brief reordering window only
static constexpr uint32_t JITTER_SKIP_MS  = 2500;  // skip hole if peer packet lost

struct VoiceAckSlot {
  uint32_t tag;
  uint32_t sent_ms;
  uint16_t len;
  uint8_t  seq;
  uint8_t  retx;
  bool     eos;
  bool     acked;
  bool     timed_out;
  uint8_t  data[180];
};

static VoiceAckSlot g_voice_acks[VOICE_ACK_SLOTS];
static int          g_voice_ack_idx = 0;
static int          g_packets_acked = 0;
static int          g_packets_lost  = 0;
static uint8_t      g_tx_seq        = 0;

static int countOpenVoiceAcks() {
  int n = 0;
  for (int i = 0; i < VOICE_ACK_SLOTS; i++) {
    if (g_voice_acks[i].tag && !g_voice_acks[i].acked && !g_voice_acks[i].timed_out)
      n++;
  }
  return n;
}

struct JitterSlot {
  bool     used;
  bool     eos;
  uint8_t  seq;
  uint16_t len;
  uint32_t rx_ms;
  uint8_t  data[180];
};

static JitterSlot   g_jitter[JITTER_DEPTH];
static uint8_t      g_rx_next_seq   = 0;
static bool         g_rx_seq_init   = false;
static uint32_t     g_rx_stream_ms  = 0;

// Missed-call / unreachable
static constexpr uint32_t MISSED_CALL_RETRY_MS   = 3UL * 60UL * 1000UL;
static constexpr int      MISSED_CALL_MAX_SENDS  = 3;

static bool     g_missed_pending   = false;
static uint8_t  g_missed_prefix[6] = {0};
static char     g_missed_name[32]  = {0};
static uint32_t g_missed_last_try  = 0;
static int      g_missed_sends     = 0;

static void missedCallClear() {
  g_missed_pending = false;
  g_missed_sends   = 0;
  g_missed_last_try = 0;
  memset(g_missed_prefix, 0, sizeof(g_missed_prefix));
  g_missed_name[0] = 0;
}

static void missedCallArmFromSession() {
  if (!g_has_target) return;
  if (g_packets_sent == 0) return;
  if (g_packets_acked > 0) {
    missedCallClear();
    return;
  }
  memcpy(g_missed_prefix, g_target.id.pub_key, 6);
  strncpy(g_missed_name, g_target.name, sizeof(g_missed_name) - 1);
  g_missed_name[sizeof(g_missed_name) - 1] = 0;
  g_missed_pending  = true;
  g_missed_sends    = 0;
  g_missed_last_try = 0;
  Serial.printf("[voice] missed-call armed for %s (sent=%d acked=0)\n",
                g_missed_name, g_packets_sent);
}

static bool missedCallSendOnce(UITask& ui) {
  if (!g_missed_pending || !ui.mesh) return false;
  if (g_missed_sends >= MISSED_CALL_MAX_SENDS) {
    missedCallClear();
    return false;
  }
  ContactInfo* c = ui.contactByPrefix(g_missed_prefix);
  if (!c) return false;

  char msg[64];
  const char* me = (ui.prefs && ui.prefs->node_name[0]) ? ui.prefs->node_name : "Someone";
  snprintf(msg, sizeof(msg), "Missed voice call from %s", me);
  bool ok = ui.sendDM(g_missed_prefix, msg);
  g_missed_last_try = millis();
  if (ok) {
    g_missed_sends++;
    Serial.printf("[voice] missed-call notify #%d -> %s\n", g_missed_sends, c->name);
  }
  return ok;
}

// Match DeckHW::i2sTone exactly (pins/rate/format) so beeps and voice share
// the same working speaker path. Codec2 PCM is often very quiet — we AGC it.
static bool spk_i2s_start() {
  if (g_spk_started) return true;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 16000;  // same as DeckHW beeps
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;     // DeckHW uses false; APLL can glitch T-Deck speaker
  cfg.tx_desc_auto_clear = true;

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[voice] spk i2s_driver_install failed: %d\n", (int)err);
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = TDECK_I2S_BCK;   // 7 — same as DeckHW.h
  pins.ws_io_num    = TDECK_I2S_WS;    // 5
  pins.data_out_num = TDECK_I2S_DOUT;  // 6
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);

  g_spk_started = true;
  Serial.println("[voice] speaker I2S started (DeckHW-compatible stereo 16k)");
  // No test click — it was heard as a buzz/click every time I2S restarted
  // between ~1 s LoRa voice packets.
  return true;
}

static void spk_i2s_stop() {
  if (!g_spk_started) return;
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  g_spk_started = false;
  Serial.println("[voice] speaker I2S stopped");
}

// Peak of mono buffer (for AGC + diagnostics)
static int pcm_peak(const int16_t* mono, int n) {
  int peak = 0;
  for (int i = 0; i < n; i++) {
    int a = mono[i];
    if (a < 0) a = -a;
    if (a > peak) peak = a;
  }
  return peak;
}

// Write stereo silence to keep I2S DMA fed across LoRa packet gaps (~1 s audio
// then airtime wait). Empty DMA + auto_clear was the periodic buzz/click.
static void spk_write_silence(int n_mono_samples) {
  if (!g_spk_started || n_mono_samples <= 0) return;
  if (g_ptt || g_tx_finishing) return;
  static const int16_t z[128] = {0};
  int i = 0;
  while (i < n_mono_samples) {
    int chunk = n_mono_samples - i;
    if (chunk > 64) chunk = 64;
    size_t wr = 0;
    // Short timeout — if DMA is full we are ahead of the clock, fine
    i2s_write(I2S_NUM_0, (const char*)z,
              (size_t)chunk * 2 * sizeof(int16_t),
              &wr, pdMS_TO_TICKS(20));
    i += chunk;
  }
}

// Expand mono → stereo. Soft AGC × call volume (1–10).
static void spk_write_mono(const int16_t* mono, int n_samples) {
  if (!g_spk_started || !mono || n_samples <= 0) return;
  // Half-duplex: never play peer while we are transmitting
  if (g_ptt || g_tx_finishing) return;

  static int32_t agc_q8 = 256 * 4;
  int peak = pcm_peak(mono, n_samples);
  int32_t target_q8 = 256 * 4;
  if (peak > 120) {
    // Aim ~12000; slow AGC so packet edges don't pump volume
    target_q8 = (12000 * 256) / peak;
    if (target_q8 > 256 * 12) target_q8 = 256 * 12;
    if (target_q8 < 256) target_q8 = 256;
  } else if (peak > 30) {
    target_q8 = 256 * 6;
  } else {
    // Near-silent: hold last gain (don't dive and then slam back up)
    target_q8 = agc_q8;
  }
  // Very slow attack/release — packet gaps were AGC-pumping every ~1 s
  agc_q8 = (agc_q8 * 15 + target_q8) / 16;

  // Call volume: 1=0.4× … 5=1.0× … 10=2.0× relative to AGC
  int vol = g_call_vol;
  if (vol < 1) vol = 1;
  if (vol > 10) vol = 10;
  int32_t gain_q8 = (agc_q8 * vol) / 5;
  if (gain_q8 > 256 * 20) gain_q8 = 256 * 20;

  int16_t stereo[128];
  size_t total_wr = 0;
  int i = 0;
  while (i < n_samples) {
    int chunk = n_samples - i;
    if (chunk > 64) chunk = 64;
    for (int k = 0; k < chunk; k++) {
      int32_t g = ((int32_t)mono[i + k] * gain_q8) >> 8;
      if (g > 32767) g = 32767;
      if (g < -32768) g = -32768;
      int16_t s = (int16_t)g;
      stereo[k * 2]     = s;
      stereo[k * 2 + 1] = s;
    }
    size_t wr = 0;
    esp_err_t e = i2s_write(I2S_NUM_0, (const char*)stereo,
                            (size_t)chunk * 2 * sizeof(int16_t),
                            &wr, pdMS_TO_TICKS(500));
    if (e != ESP_OK) {
      Serial.printf("[voice] i2s_write err=%d at sample %d\n", (int)e, i);
      break;
    }
    total_wr += wr;
    i += chunk;
  }
  g_last_rx_play_ms = millis();
  static uint32_t last_spk_log;
  if (millis() - last_spk_log > 1500) {
    last_spk_log = millis();
    Serial.printf("[voice] spk n=%d peak=%d gain=%d/256 vol=%d wr=%u\n",
                  n_samples, peak, (int)gain_q8, vol, (unsigned)total_wr);
  }
}

static bool rx_enqueue(const uint8_t* data, size_t len, bool eos,
                       bool from_contact, const char* name) {
  if (len == 0 || len > RX_PKT_MAX) return false;
  if (g_rx_count >= RX_QUEUE_DEPTH) {
    g_rx_tail = (g_rx_tail + 1) % RX_QUEUE_DEPTH;
    g_rx_count--;
  }

  RxVoicePkt& p = g_rx_q[g_rx_head];
  memcpy(p.data, data, len);
  p.len = (uint16_t)len;
  p.eos = eos;
  p.from_contact = from_contact;
  if (name) {
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = 0;
  } else {
    p.name[0] = 0;
  }

  g_rx_head = (g_rx_head + 1) % RX_QUEUE_DEPTH;
  g_rx_count++;
  return true;
}

static void jitterReset() {
  memset(g_jitter, 0, sizeof(g_jitter));
  g_rx_seq_init  = false;
  g_rx_next_seq  = 0;
  g_rx_stream_ms = 0;
}

static bool jitterInsert(uint8_t seq, const uint8_t* payload, uint16_t len, bool eos) {
  for (int i = 0; i < JITTER_DEPTH; i++) {
    if (g_jitter[i].used && g_jitter[i].seq == seq) return false;
  }
  int slot = -1;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < JITTER_DEPTH; i++) {
    if (!g_jitter[i].used) { slot = i; break; }
    if (g_jitter[i].rx_ms < oldest) {
      oldest = g_jitter[i].rx_ms;
      slot = i;
    }
  }
  if (slot < 0) return false;

  JitterSlot& j = g_jitter[slot];
  j.used  = true;
  j.eos   = eos;
  j.seq   = seq;
  j.len   = len;
  j.rx_ms = millis();
  if (len > sizeof(j.data)) len = sizeof(j.data);
  memcpy(j.data, payload, len);
  j.len = len;

  if (!g_rx_seq_init) {
    g_rx_next_seq  = seq;
    g_rx_seq_init  = true;
    g_rx_stream_ms = millis();
  }
  return true;
}

static int jitterCount() {
  int n = 0;
  for (int i = 0; i < JITTER_DEPTH; i++) if (g_jitter[i].used) n++;
  return n;
}

static bool jitterTakeNext(JitterSlot& out) {
  for (int i = 0; i < JITTER_DEPTH; i++) {
    if (g_jitter[i].used && g_jitter[i].seq == g_rx_next_seq) {
      out = g_jitter[i];
      g_jitter[i].used = false;
      g_rx_next_seq++;
      return true;
    }
  }
  return false;
}

static bool rx_dequeue(RxVoicePkt& out) {
  if (g_rx_count <= 0) return false;
  out = g_rx_q[g_rx_tail];
  g_rx_tail = (g_rx_tail + 1) % RX_QUEUE_DEPTH;
  g_rx_count--;
  return true;
}

// ============================================================
// RX path (half-duplex):
//   c2rx  — jitter only, small stack (~4–6 KB)
//   c2dec — codec2_decode + speaker, large stack (~20–24 KB INTERNAL)
//   c2work— encode while PTT; not concurrent with c2dec (share internal RAM)
// Loop never runs codec2_decode (overflowed at 24 KB with UI overhead).
// ============================================================

static void voiceRxTask(void* /*param*/) {
  Serial.println("[voice] RX task started (jitter only, no codec2)");

  while (true) {
    RxVoicePkt pkt;
    bool any = false;
    while (rx_dequeue(pkt)) {
      any = true;
      if (pkt.len < 2) continue;
      uint8_t flags = pkt.data[0];
      uint8_t seq   = pkt.data[1];
      bool eos      = (flags & VOICE_FLAG_EOS) != 0;
      const uint8_t* payload = pkt.data + 2;
      uint16_t plen = pkt.len - 2;
      jitterInsert(seq, payload, plen, eos);
      Serial.printf("[voice] jitter insert seq=%u len=%u eos=%d depth=%d\n",
                    seq, plen, eos ? 1 : 0, jitterCount());
    }
    vTaskDelay(pdMS_TO_TICKS(any ? 5 : 15));
  }
}

// PCM for decode lives in PSRAM (not on task stack)
static bool ensureRxPcm() {
  if (g_rx_pcm) return true;
  g_rx_pcm = (int16_t*)heap_caps_malloc(
      40 * PCM16_SAMPLES * sizeof(int16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_rx_pcm) {
    g_rx_pcm = (int16_t*)heap_caps_malloc(
        40 * PCM16_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!g_rx_pcm) {
    Serial.println("[voice] RX PCM alloc failed");
    return false;
  }
  Serial.printf("[voice] RX PCM buffer %u B @ %p\n",
                (unsigned)(40 * PCM16_SAMPLES * sizeof(int16_t)), (void*)g_rx_pcm);
  return true;
}

// One packet of decode+play; called only from c2dec task
static void decPlayOne() {
  // Half-duplex: never decode/play while local TX holds the floor
  if (g_call_state != CALL_CONNECTED || g_ptt || g_tx_finishing || !g_c2.ready())
    return;

  bool can_play = false;
  if (jitterCount() >= JITTER_PLAY_THRESH) can_play = true;
  if (g_rx_seq_init && (millis() - g_rx_stream_ms) > JITTER_WAIT_MS &&
      jitterCount() > 0)
    can_play = true;
  if (!can_play) return;

  JitterSlot next;
  if (!jitterTakeNext(next)) {
    // Hole in sequence: wait longer before skip (LoRa loss is common)
    if (g_rx_seq_init && (millis() - g_rx_stream_ms) > JITTER_SKIP_MS &&
        jitterCount() > 0) {
      Serial.printf("[voice] jitter skip missing seq=%u (have %d buffered)\n",
                    g_rx_next_seq, jitterCount());
      g_rx_next_seq++;
      g_rx_stream_ms = millis();  // reset skip timer for next hole
    }
    return;
  }
  g_rx_stream_ms = millis();  // activity keeps skip timer calm

  if (!ensureRxPcm()) return;
  if (!spk_i2s_start()) {
    Serial.println("[voice] RX: speaker start failed");
    return;
  }

  // Decode + play one Codec2 frame at a time (lower peak stack than whole packet)
  const int bpf = g_c2.bytesPerFrame();
  if (bpf <= 0 || !next.data || next.len < (uint16_t)bpf) {
    Serial.println("[voice] RX: bad frame size");
    return;
  }
  int frames = (int)next.len / bpf;
  int played = 0;
  for (int f = 0; f < frames; f++) {
    if (g_ptt || g_tx_finishing || g_call_state != CALL_CONNECTED) break;
    int samples = g_c2.decodeFrame(next.data + f * bpf, g_rx_pcm);
    if (samples <= 0) continue;
    spk_write_mono(g_rx_pcm, samples);
    played += samples;
    // Rare yield only — frequent vTaskDelay caused I2S underrun ticks mid-packet
    if ((f & 15) == 15) vTaskDelay(1);
  }

  {
    static bool logged_hwm = false;
    if (!logged_hwm && played > 0) {
      logged_hwm = true;
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
      Serial.printf(
          "[voice] c2dec after 1st decode: stack_hwm=%u words (~%u B free) "
          "— if <200 words, need bigger stack\n",
          (unsigned)hwm, (unsigned)(hwm * sizeof(StackType_t)));
    }
  }

  if (played > 0) {
    Serial.printf("[voice] RX played seq=%u samples=%d frames=%d eos=%d\n",
                  next.seq, played, frames, next.eos ? 1 : 0);
  }

  if (next.eos) {
    // Drain a little silence then stop — end of talk burst only
    spk_write_silence(320);
    vTaskDelay(pdMS_TO_TICKS(40));
    spk_i2s_stop();
    jitterReset();
    g_last_rx_play_ms = 0;  // leave HEARING promptly after talk-burst end
  }
}

static void voiceDecTask(void* /*param*/) {
  Serial.println("[voice] c2dec started (codec2_decode + speaker)");
  if (!ensureRxPcm()) {
    Serial.println("[voice] c2dec: no PCM buffer, exiting");
    g_dec_worker_run = false;
    g_dec_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  while (g_dec_worker_run) {
    if (g_call_state == CALL_CONNECTED && !g_ptt && !g_tx_finishing &&
        g_c2.ready())
      decPlayOne();

    // Between LoRa media packets (~0.96 s audio, then airtime+ACK gap): keep
    // DMA fed with silence so underrun doesn't click. Only stop after a long
    // idle (lost EOS / peer done), not after every packet.
    if (g_spk_started && g_last_rx_play_ms &&
        g_call_state == CALL_CONNECTED && !g_ptt && !g_tx_finishing) {
      uint32_t idle = millis() - g_last_rx_play_ms;
      if (jitterCount() == 0) {
        if (idle < 3000) {
          // ~20 ms of mono silence per loop (~10 ms task period)
          spk_write_silence(320);
        } else {
          spk_i2s_stop();
          g_last_rx_play_ms = 0;
          Serial.println("[voice] c2dec: long idle — speaker off");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  Serial.println("[voice] c2dec exit");
  g_dec_task = nullptr;
  vTaskDelete(nullptr);
}

static StackType_t* allocInternalStack(uint32_t bytes, const char* tag);

// Heap / stack budget dump. Use before/after any heavy alloc so serial logs
// answer: "is internal RAM the bottleneck?" vs "need lower codec mode?".
static void voiceMem(const char* tag) {
  multi_heap_info_t ii{}, is{};
  heap_caps_get_info(&ii, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&is, MALLOC_CAP_SPIRAM);

  // FreeRTOS task stacks must be INTERNAL (not PSRAM).
  // Half-duplex: one shared HD stack (~18–24 KB) for c2dec XOR c2work; c2rx ~3–4 KB.
  const size_t largest = ii.largest_free_block;
  const size_t budget  = largest;
  const uint32_t need_rx  = 3072;
  const uint32_t need_tx  = 12288;
  const uint32_t need_dec = 18432;
  (void)need_tx;

  UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf(
    "[voice-mem] %s\n"
    "  free_heap=%u min_ever=%u\n"
    "  INTERNAL free=%u largest=%u min_ever=%u\n"
    "  SPIRAM   free=%u largest=%u\n"
    "  int_stack_budget=%u  need_c2rx>=%u need_hd>=%u  hd=%u B @ %p  %s\n"
    "  stacks c2=%p voice=%p rx=%p dec=%p  ptt=%d ready=%d state=%d\n"
    "  this_task_hwm=%u words (~%u bytes free on this task stack)\n",
    tag,
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    (unsigned)ii.total_free_bytes, (unsigned)largest,
    (unsigned)ii.minimum_free_bytes,
    (unsigned)is.total_free_bytes, (unsigned)is.largest_free_block,
    (unsigned)budget, need_rx, need_dec,
    (unsigned)g_hd_stack_bytes, (void*)g_hd_stack,
    g_hd_stack ? "HD stack held"
               : ((budget >= need_dec) ? "OK to alloc HD"
                                       : "SHORT for HD stack"),
    (void*)g_c2_stack, (void*)g_voice_stack, (void*)g_rx_stack, (void*)g_dec_stack,
    g_ptt ? 1 : 0, g_c2.ready() ? 1 : 0, (int)g_call_state,
    (unsigned)hwm, (unsigned)(hwm * sizeof(StackType_t)));
}

// Try each candidate size; log every skip/fail so serial shows the ceiling.
static StackType_t* allocInternalStackFromList(const uint32_t* cands, int ncand,
                                               size_t budget, const char* tag,
                                               uint32_t* out_bytes) {
  Serial.printf("[voice] %s: pick INTERNAL stack  budget=%u largest_int=%u\n",
                tag, (unsigned)budget,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  for (int i = 0; i < ncand; i++) {
    uint32_t cand = cands[i] & ~0xFu;
    if (cand > budget) {
      Serial.printf("[voice] %s: skip %u B (>%u budget)\n",
                    tag, (unsigned)cand, (unsigned)budget);
      continue;
    }
    StackType_t* p = allocInternalStack(cand, tag);
    if (p) {
      if (out_bytes) *out_bytes = cand;
      Serial.printf("[voice] %s: chose %u B  remaining_largest=%u\n",
                    tag, (unsigned)cand,
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
      return p;
    }
    Serial.printf("[voice] %s: alloc %u B failed (frag?)\n", tag, (unsigned)cand);
  }
  Serial.printf("[voice] %s: ALL candidates failed  budget=%u\n",
                tag, (unsigned)budget);
  if (out_bytes) *out_bytes = 0;
  return nullptr;
}

static StackType_t* allocTaskStack(uint32_t bytes, const char* tag) {
  StackType_t* p = (StackType_t*)heap_caps_malloc(
      bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) {
    Serial.printf("[voice] %s stack %u B in SPIRAM @ %p\n",
                  tag, (unsigned)bytes, (void*)p);
    return p;
  }
  p = (StackType_t*)heap_caps_malloc(
      bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) {
    Serial.printf("[voice] %s stack %u B FALLBACK internal @ %p\n",
                  tag, (unsigned)bytes, (void*)p);
  } else {
    Serial.printf("[voice] %s stack ALLOC FAILED (%u B)\n", tag, (unsigned)bytes);
  }
  return p;
}

static void codec2InitTask(void* /*param*/) {
  voiceMem("c2init: before begin");
  bool ok = g_c2.begin();
  voiceMem(ok ? "c2init: after begin OK" : "c2init: after begin FAIL");
  Serial.printf("[voice] codec2 init %s ready=%d heap=%u\n",
                ok ? "OK" : "FAIL", g_c2.ready() ? 1 : 0,
                (unsigned)ESP.getFreeHeap());
  {
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    Serial.printf("[voice] c2init stack_hwm=%u words (~%u B free)\n",
                  (unsigned)hwm, (unsigned)(hwm * sizeof(StackType_t)));
  }
  // Mark done first; clear task handle last so freeCodec2InitStack never races
  g_c2_init_ok = ok;
  g_c2_init_done = true;
  g_c2_task = nullptr;
  vTaskDelete(nullptr);
}

static StackType_t* allocInternalStack(uint32_t bytes, const char* tag) {
  // Task stacks for codec2 MUST be internal DRAM (not PSRAM).
  StackType_t* p = (StackType_t*)heap_caps_malloc(
      bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) {
    Serial.printf("[voice] %s stack %u B INTERNAL @ %p  (after: largest_int=%u)\n",
                  tag, (unsigned)bytes, (void*)p,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  } else {
    Serial.printf(
        "[voice] %s stack ALLOC FAILED (%u B)  largest_int=%u free_int=%u\n",
        tag, (unsigned)bytes,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  return p;
}

static void freeCodec2InitStack() {
  // NEVER free the stack under a live c2init task (causes canary/reboot).
  if (g_c2_task) {
    Serial.println("[voice] freeCodec2InitStack: skip — c2init still running");
    return;
  }
  if (g_c2_stack) {
    heap_caps_free(g_c2_stack);
    g_c2_stack = nullptr;
  }
}

static bool ensureRxWorker() {
  if (g_rx_task) return true;

  // Do not allocate / free stacks while codec2_create is using c2init stack
  if (g_c2_task) {
    Serial.println("[voice] ensureRxWorker: defer — c2init still running");
    return false;
  }
  if (g_c2_stack) {
    Serial.println("[voice] ensureRxWorker: free idle c2init stack");
    freeCodec2InitStack();
  }
  // Grab large HD stack before small c2rx so we never fragment the big block
  if (g_call_state == CALL_CONNECTED && g_c2.ready() && !g_hd_stack) {
    ensureHdStack();  // may recreate RX if it had to free it — re-check below
  }
  if (g_rx_task) return true;
  voiceMem("ensureRxWorker: before stack alloc (jitter-only c2rx)");

  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  size_t budget  = largest;  // jitter task is tiny — leave max free for c2dec later

  // Prefer small RX stacks so half-duplex turnaround can still fit c2dec
  const uint32_t candidates[] = { 4096, 3584, 3072 };
  uint32_t stack_bytes = 0;
  g_rx_stack = allocInternalStackFromList(
      candidates, (int)(sizeof(candidates) / sizeof(candidates[0])),
      budget, "voice-rx", &stack_bytes);

  if (!g_rx_stack) {
    voiceMem("ensureRxWorker: FAILED — no internal stack for c2rx");
    return false;
  }

  g_rx_task = xTaskCreateStaticPinnedToCore(
      voiceRxTask, "c2rx", stack_bytes / sizeof(StackType_t),
      nullptr, 1, g_rx_stack, &g_rx_task_buf, 1);
  if (!g_rx_task) {
    heap_caps_free(g_rx_stack);
    g_rx_stack = nullptr;
    Serial.printf("[voice] c2rx xTaskCreateStatic FAILED (stack %u B)\n",
                  (unsigned)stack_bytes);
    return false;
  }
  Serial.printf("[voice] c2rx started stack=%u B (jitter only)\n",
                (unsigned)stack_bytes);
  voiceMem("ensureRxWorker: OK after create");
  return true;
}

static void stopRxWorker() {
  if (!g_rx_task && !g_rx_stack) return;
  Serial.println("[voice] stopRxWorker");
  // RX task has no run flag — delete and free stack (jitter queue stays)
  if (g_rx_task) {
    vTaskDelete(g_rx_task);
    g_rx_task = nullptr;
  }
  if (g_rx_stack) {
    heap_caps_free(g_rx_stack);
    g_rx_stack = nullptr;
  }
}

// codec2_decode needs a large INTERNAL stack. 16 KB overflows (canary).
// Shared HD pool: ~18 KB min; prefer ~20–24 KB. NEVER take the whole free
// heap — I2S mic/spk DMA also needs contiguous INTERNAL (was 42 KB HD →
// free_heap≈4 KB → i2s_alloc_dma_buffer failed, no TX audio).
static constexpr uint32_t C2DEC_STACK_MIN  = 18432;  // 18 KB hard floor
static constexpr uint32_t C2DEC_STACK_PREF = 20480;  // 20 KB is enough + leaves DMA room
static constexpr uint32_t C2DEC_STACK_MAX  = 24576;  // hard cap; never larger
static constexpr uint32_t C2TX_STACK_MIN   = 10240;
// Leave room for I2S DMA (mic + spk ~8–16 KB) + small c2rx + heap slack
static constexpr uint32_t INTERNAL_DMA_RESERVE = 16384;
static uint32_t g_dec_fail_until_ms = 0;  // backoff so we don't thrash RX/dec

// Stop decode task only — keep g_hd_stack for the rest of the call.
static void stopDecWorker() {
  if (!g_dec_task) {
    g_dec_stack = nullptr;
    return;
  }
  Serial.println("[voice] stopDecWorker (keep HD stack)");
  g_dec_worker_run = false;
  for (int i = 0; i < 50 && g_dec_task; i++)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (g_dec_task) {
    vTaskDelete(g_dec_task);
    g_dec_task = nullptr;
  }
  g_dec_stack = nullptr;  // alias only; g_hd_stack stays allocated
}

// Stop TX task only — keep g_hd_stack for reverse decode.
static void stopVoiceWorker() {
  if (!g_voice_task) {
    g_voice_stack = nullptr;
    if (g_mic_started) {
      mic_i2s_stop();
      g_mic_started = false;
    }
    return;
  }
  Serial.println("[voice] stopVoiceWorker (keep HD stack)");
  g_voice_worker_run = false;
  // Do NOT clear g_ptt here — that was killing PTT when ensureDecWorker ran
  for (int i = 0; i < 50 && g_voice_task; i++)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (g_voice_task) {
    vTaskDelete(g_voice_task);
    g_voice_task = nullptr;
  }
  g_voice_stack = nullptr;
  if (g_mic_started) {
    mic_i2s_stop();
    g_mic_started = false;
  }
}

static void freeHdStack() {
  // Must only run when neither TX nor decode task is using the buffer.
  if (g_dec_task || g_voice_task) {
    Serial.println("[voice] freeHdStack: tasks still alive — stop first");
    stopDecWorker();
    stopVoiceWorker();
  }
  if (g_hd_stack) {
    Serial.printf("[voice] freeHdStack %u B @ %p\n",
                  (unsigned)g_hd_stack_bytes, (void*)g_hd_stack);
    heap_caps_free(g_hd_stack);
    g_hd_stack = nullptr;
    g_hd_stack_bytes = 0;
  }
  g_dec_stack = nullptr;
  g_voice_stack = nullptr;
}

// Allocate the shared half-duplex stack once. Call at connect (before c2rx)
// so the largest free INTERNAL block is still intact.
static bool ensureHdStack() {
  if (g_hd_stack && g_hd_stack_bytes >= C2DEC_STACK_MIN)
    return true;

  // Wait for codec2 init — do not steal its stack or race heap under it
  if (g_c2_task || (g_c2_init_req && !g_c2_init_done)) {
    Serial.println("[voice] ensureHdStack: defer until c2init finishes");
    return false;
  }
  freeCodec2InitStack();

  // Temporarily drop small jitter task so its 3–4 KB can coalesce if adjacent.
  bool had_rx = (g_rx_task != nullptr || g_rx_stack != nullptr);
  if (had_rx) {
    Serial.println("[voice] ensureHdStack: free c2rx for contiguous RAM");
    stopRxWorker();
  }
  // Neither worker may hold the buffer while we (re)alloc
  stopDecWorker();
  stopVoiceWorker();
  vTaskDelay(pdMS_TO_TICKS(5));
  voiceMem("ensureHdStack: before alloc");

  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  // Cap budget: leave DMA headroom + never exceed HD max
  size_t budget = largest;
  if (budget > INTERNAL_DMA_RESERVE)
    budget -= INTERNAL_DMA_RESERVE;
  else
    budget = 0;
  if (budget > C2DEC_STACK_MAX)
    budget = C2DEC_STACK_MAX;

  Serial.printf(
      "[voice] ensureHdStack: largest=%u reserve=%u budget=%u (max HD=%u)\n",
      (unsigned)largest, (unsigned)INTERNAL_DMA_RESERVE, (unsigned)budget,
      (unsigned)C2DEC_STACK_MAX);

  uint32_t candidates[8];
  int nc = 0;
  // Fixed sizes only (descending). Do NOT floor(largest) — that ate 42 KB.
  const uint32_t prefs[] = {
      C2DEC_STACK_MAX, C2DEC_STACK_PREF, 19456, 19200, C2DEC_STACK_MIN
  };
  for (unsigned pi = 0; pi < sizeof(prefs) / sizeof(prefs[0]); pi++) {
    uint32_t s = prefs[pi] & ~0xFu;
    if (s > budget || s < C2DEC_STACK_MIN || s > C2DEC_STACK_MAX) continue;
    bool dup = false;
    for (int j = 0; j < nc; j++) if (candidates[j] == s) { dup = true; break; }
    if (!dup) candidates[nc++] = s;
  }
  // Only when RAM is tight (budget < PREF): try largest usable under budget
  if (budget >= C2DEC_STACK_MIN && budget < C2DEC_STACK_PREF) {
    uint32_t floor_sz = ((uint32_t)budget) & ~0xFu;
    if (floor_sz > 64) floor_sz -= 64;
    floor_sz &= ~0xFu;
    if (floor_sz >= C2DEC_STACK_MIN && floor_sz <= budget &&
        floor_sz <= C2DEC_STACK_MAX) {
      bool dup = false;
      for (int j = 0; j < nc; j++) if (candidates[j] == floor_sz) { dup = true; break; }
      if (!dup && nc < 8) candidates[nc++] = floor_sz;
    }
  }
  for (int i = 0; i < nc; i++) {
    for (int j = i + 1; j < nc; j++) {
      if (candidates[j] > candidates[i]) {
        uint32_t t = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = t;
      }
    }
  }

  uint32_t stack_bytes = 0;
  g_hd_stack = (nc > 0)
      ? allocInternalStackFromList(candidates, nc, budget, "voice-hd", &stack_bytes)
      : nullptr;

  if (!g_hd_stack || stack_bytes < C2DEC_STACK_MIN) {
    if (g_hd_stack) {
      heap_caps_free(g_hd_stack);
      g_hd_stack = nullptr;
    }
    g_hd_stack_bytes = 0;
    if (had_rx) ensureRxWorker();
    g_dec_fail_until_ms = millis() + 2000;
    static uint32_t last_fail_log;
    if (millis() - last_fail_log > 2000) {
      last_fail_log = millis();
      voiceMem("ensureHdStack: FAILED");
      Serial.printf(
          "[voice] DIAG: need >=%u B contiguous INTERNAL for shared HD stack; "
          "largest=%u\n",
          (unsigned)C2DEC_STACK_MIN, (unsigned)largest);
    }
    return false;
  }

  g_hd_stack_bytes = stack_bytes;
  g_dec_fail_until_ms = 0;
  Serial.printf("[voice] HD stack held %u B @ %p (shared c2dec/c2work)\n",
                (unsigned)g_hd_stack_bytes, (void*)g_hd_stack);
  if (had_rx) ensureRxWorker();
  voiceMem("ensureHdStack: OK");
  return true;
}

// Large-stack decode task. Half-duplex: never steals the TX path while PTT.
static bool ensureDecWorker() {
  if (g_dec_task) return true;

  // Talking / finishing TX: leave HD stack for encode
  if (g_ptt || g_tx_finishing || g_voice_task) {
    return false;
  }
  if (g_dec_fail_until_ms && (int32_t)(millis() - g_dec_fail_until_ms) < 0)
    return false;

  freeCodec2InitStack();
  stopVoiceWorker();  // task only; HD buffer retained

  if (!ensureHdStack()) {
    return false;
  }

  g_dec_stack = g_hd_stack;
  g_dec_worker_run = true;
  g_dec_task = xTaskCreateStaticPinnedToCore(
      voiceDecTask, "c2dec", g_hd_stack_bytes / sizeof(StackType_t),
      nullptr, 1, g_dec_stack, &g_dec_task_buf, 1);
  if (!g_dec_task) {
    g_dec_worker_run = false;
    g_dec_stack = nullptr;
    g_dec_fail_until_ms = millis() + 2000;
    Serial.printf("[voice] c2dec xTaskCreateStatic FAILED (stack %u B)\n",
                  (unsigned)g_hd_stack_bytes);
    return false;
  }

  Serial.printf("[voice] c2dec started stack=%u B internal (shared HD)\n",
                (unsigned)g_hd_stack_bytes);
  ensureRxWorker();
  voiceMem("ensureDecWorker: OK after create");
  return true;
}

static bool ensureVoiceWorker() {
  if (g_voice_task) return true;

  freeCodec2InitStack();
  // Half-duplex: stop decode (same HD buffer reused for encode)
  stopDecWorker();
  spk_i2s_stop();
  voiceMem("ensureVoiceWorker: before HD reuse");

  if (!ensureHdStack()) {
    voiceMem("ensureVoiceWorker: FAILED — no HD stack");
    return false;
  }
  if (g_hd_stack_bytes < C2TX_STACK_MIN) {
    voiceMem("ensureVoiceWorker: FAILED — HD stack too small for TX");
    return false;
  }

  uint32_t stack_bytes = g_hd_stack_bytes;
  g_voice_stack = g_hd_stack;

  uint32_t words = stack_bytes / sizeof(StackType_t);
  g_voice_worker_run = true;
  g_voice_task = xTaskCreateStaticPinnedToCore(
      voiceWorkerTask, "c2work", words,
      nullptr, 1,
      g_voice_stack, &g_voice_task_buf, 1);

  if (!g_voice_task) {
    g_voice_worker_run = false;
    g_voice_stack = nullptr;  // HD buffer retained for retry / decode
    Serial.printf("[voice] c2work xTaskCreateStatic FAILED (stack %u B)\n",
                  (unsigned)stack_bytes);
    return false;
  }
  Serial.printf("[voice] c2work started stack=%u B (shared HD)\n",
                (unsigned)stack_bytes);
  voiceMem("ensureVoiceWorker: OK after create");
  return true;
}

static void voiceWorkerTask(void* /*param*/) {
  Serial.printf("[voice] worker start  heap=%u  largest_int=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  while (g_voice_worker_run) {
    if (!g_c2.ready()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Only encode while we are CONNECTED and PTT is held
    if (g_call_state != CALL_CONNECTED || !g_ptt) {
      if (g_mic_started) {
        mic_i2s_stop();
        g_mic_started = false;
        Serial.println("[voice] mic stopped (not connected or PTT released)");
      }
      if (g_flush_req) {
        g_flush_req = false;
        if (g_pending_len == 0 && g_c2.flush()) {
          int n = (int)g_c2.packetLen();
          if (n > 0 && n <= (int)sizeof(g_pending_pkt)) {
            memcpy(g_pending_pkt, g_c2.packetData(), n);
            g_pending_eos = true;
            g_pending_len = n;
          }
          g_c2.clearPacket();
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Final flush requested from key() on PTT release
    if (g_flush_req) {
      g_flush_req = false;
      if (g_pending_len == 0 && g_c2.flush()) {
        int n = (int)g_c2.packetLen();
        if (n > 0 && n <= (int)sizeof(g_pending_pkt)) {
          memcpy(g_pending_pkt, g_c2.packetData(), n);
          g_pending_eos = true;
          g_pending_len = n;
        }
        g_c2.clearPacket();
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    // First time we see PTT while CONNECTED: start I2S + ES7210
    if (!g_mic_started) {
      Serial.printf("[voice] starting mic I2S  to=%s\n", g_target.name);

      // MCLK must be running before ES7210 register config
      if (!mic_i2s_start()) {
        Serial.println("[voice] mic_i2s_start failed");
        g_ptt = false;
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      vTaskDelay(pdMS_TO_TICKS(50));
      es7210_config();
      vTaskDelay(pdMS_TO_TICKS(50));

      // Flush settle / DC transient (mono ALL_LEFT frames)
      static int16_t settle[512];
      size_t junk = 0;
      int settle_peak = 0;
      for (int i = 0; i < 12; i++) {
        if (i2s_read(I2S_NUM_1, (char*)settle, sizeof(settle),
                     &junk, pdMS_TO_TICKS(50)) == ESP_OK) {
          int n = (int)(junk / sizeof(int16_t));
          for (int j = 0; j < n; j++) {
            int a = settle[j];
            if (a < 0) a = -a;
            if (a > settle_peak) settle_peak = a;
          }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      Serial.printf("[voice] mic settle peak=%d (want >> 100 if mic live)\n",
                    settle_peak);

      if (!g_frame) {
        Serial.println("[voice] FATAL: g_frame is null");
        g_ptt = false;
        mic_i2s_stop();
        continue;
      }

      g_mic_started = true;
      Serial.println("[voice] mic ready");
      continue;
    }

    if (!g_frame) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (g_pending_len != 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    size_t br = 0;
    const size_t need = FRAME_SAMPLES_16K * sizeof(int16_t);
    esp_err_t err = i2s_read(I2S_NUM_1, (char*)g_frame, need,
                             &br, pdMS_TO_TICKS(80));

    if (err != ESP_OK || br < need) {
      static uint32_t last_rd_err;
      if (millis() - last_rd_err > 1000) {
        last_rd_err = millis();
        Serial.printf("[voice] mic i2s_read err=%d br=%u need=%u\n",
                      (int)err, (unsigned)br, (unsigned)need);
      }
      continue;
    }

    // DC block + soft clip. Hardware PGA is already high; avoid extra ×gain
    // that clips and wrecks Codec2 (sounds like static).
    {
      static int32_t dc_acc = 0;
      int peak = 0;
      for (int i = 0; i < FRAME_SAMPLES_16K; i++) {
        int32_t x = g_frame[i];
        // one-pole DC estimate
        dc_acc += (x - (dc_acc >> 8));
        x -= (dc_acc >> 8);
        if (x > 30000) x = 30000;
        if (x < -30000) x = -30000;
        g_frame[i] = (int16_t)x;
        int a = (int)x;
        if (a < 0) a = -a;
        if (a > peak) peak = a;
      }
      static uint32_t last_pk;
      if (millis() - last_pk > 500) {
        last_pk = millis();
        Serial.printf("[voice] mic peak=%d\n", peak);
      }
    }

    if (g_c2.encode16k(g_frame, FRAME_SAMPLES_16K)) {
      int n = (int)g_c2.packetLen();
      if (n > 0 && n <= (int)sizeof(g_pending_pkt)) {
        memcpy(g_pending_pkt, g_c2.packetData(), n);
        g_pending_eos = false;
        g_pending_len = n;
        Serial.printf("[voice] packet ready %d bytes\n", n);
      }
      g_c2.clearPacket();
    }
  }

  if (g_mic_started) {
    mic_i2s_stop();
    g_mic_started = false;
  }
  g_voice_task = nullptr;
  vTaskDelete(nullptr);
}

static void es_w(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(g_es_addr);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t es_r(uint8_t reg) {
  Wire.beginTransmission(g_es_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)g_es_addr, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

static void i2c_scan() {
  Serial.println("[voice] I2C scan:");
  for (uint8_t a = 1; a < 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[voice]   0x%02X ACK\n", a);
    }
  }
}

static bool es7210_probe() {
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  delay(50);
  i2c_scan();
  for (uint8_t a = 0x40; a <= 0x43; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { g_es_addr = a; return true; }
  }
  return false;
}

// T-Deck / CYPHER-M8K ES7210 bring-up (ESP-ADF es7210.c + LilyGo Microphone.ino).
//
// Bug we hit: reg01=0x34 leaves ADC34 clocks gated off. ESP-ADF mic_select for
// MIC3|MIC4 does update_reg_bit(CLOCK_OFF, mask=0x15, 0) — without that, MIC3/4
// power/gain can look correct on I2C but SDOUT stays digital silence (peak=0).
// Physical dual MEMS on T-Deck family need MIC3|MIC4 clocks + power.
static bool es7210_config() {
  // --- es7210_adc_init ---
  es_w(0x00, 0xFF); es_w(0x00, 0x41);          // reset
  es_w(0x01, 0x3F);                             // all clocks off
  es_w(0x09, 0x30); es_w(0x0A, 0x30);           // time control
  es_w(0x23, 0x2A); es_w(0x22, 0x0A);           // ADC12 HPF
  es_w(0x20, 0x0A); es_w(0x21, 0x2A);           // ADC34 HPF
  es_w(0x08, 0x14);                             // slave (clear master bit)
  es_w(0x40, 0x43);                             // analog / VMID
  es_w(0x41, 0x70); es_w(0x42, 0x70);           // MIC12 + MIC34 bias 2.87 V
  es_w(0x07, 0x20);                             // OSR
  // 16 kHz @ MCLK=4.096 MHz (256×fs): coeff row {4096000,16000,...} → reg02=0xC1
  es_w(0x02, 0xC1);
  es_w(0x04, 0x01); es_w(0x05, 0x00);           // LRCK div

  // 16-bit I2S normal
  es_w(0x11, 0x60);
  es_w(0x12, 0x00);

  // --- es7210_start + mic_select(MIC1|2|3|4) ---
  // CLOCK_OFF: clear 0x0B (MIC12) and 0x15 (MIC34) from 0x3F → 0x20
  // (0x34 left bit2+bit4 set → ADC34 stayed off → silence)
  es_w(0x01, 0x20);
  es_w(0x06, 0x00);                             // power-down off
  es_w(0x40, 0x43);
  es_w(0x47, 0x08); es_w(0x48, 0x08);           // MIC1/2 power
  es_w(0x49, 0x08); es_w(0x4A, 0x08);           // MIC3/4 power

  // Power domains then enable PGA (bit4) + gain nibble
  es_w(0x4B, 0xFF); es_w(0x4C, 0xFF);
  es_w(0x4B, 0x00);                             // MIC12 up
  es_w(0x4C, 0x00);                             // MIC34 up
  // LilyGo: MIC1|2 @ 0 dB, MIC3|4 @ 37.5 dB (GAIN_37_5DB ≈ 0x0E)
  es_w(0x43, 0x10);
  es_w(0x44, 0x10);
  es_w(0x45, 0x1E);
  es_w(0x46, 0x1E);

  es_w(0x14, 0x00); es_w(0x15, 0x00);           // unmute
  es_w(0x12, 0x00);                             // SDOUT active
  es_w(0x01, 0x20);                             // clocks stay enabled

  uint8_t r01 = es_r(0x01), r45 = es_r(0x45), r4c = es_r(0x4C);
  Serial.printf(
      "[voice] ES7210 cfg @0x%02X reg01=0x%02X (want 0x20) reg45=0x%02X reg4C=0x%02X\n",
      g_es_addr, r01, r45, r4c);
  if (r01 != 0x20)
    Serial.printf("[voice] WARN: reg01=0x%02X — ADC clocks may still be gated\n", r01);
  return true;
}

// Mic I2S: true mono 16 kHz. ALL_LEFT on ESP32 legacy I2S still packs stereo
// slots (L,L,L,L…) so a 640-sample read is only ~20 ms of real time while
// Codec2 expects 40 ms → half-speed / garbled decode on the far end.
// ONLY_LEFT delivers one sample per period = correct 16 kHz mono.
// Pins match LilyGo BOARD_ES7210_* (CYPHER-M8K same).
static bool mic_i2s_start() {
  // Ignore "not installed" on first call
  i2s_driver_uninstall(I2S_NUM_1);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;

  esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[voice] mic i2s_driver_install failed: %d\n", (int)err);
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = ES_MCLK;   // 48 — BOARD_ES7210_MCLK
  pins.bck_io_num = ES_SCK;    // 47 — BOARD_ES7210_SCK
  pins.ws_io_num = ES_LRCK;    // 21 — BOARD_ES7210_LRCK
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = ES_DIN;   // 14 — BOARD_ES7210_DIN
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
  Serial.println("[voice] mic I2S started (16k ONLY_LEFT mono, T-Deck/CYPHER-M8K)");
  return true;
}

static void mic_i2s_stop() { i2s_driver_uninstall(I2S_NUM_1); }

// ── Control helpers ───────────────────────────────────────────

bool VoiceScreen::sendVoiceControl(UITask& ui, const ContactInfo& to, uint8_t ctrl) {
  if (!ui.mesh) return false;
  uint8_t framed[4];
  framed[0] = VOICE_FLAG_CTRL | (ctrl & 0x0F);
  framed[1] = 0; // seq unused for control
  bool ok = ui.mesh->sendVoiceToContact(to, framed, 2, false);
  Serial.printf("[voice] CTRL %u -> %s ok=%d\n", ctrl, to.name, ok ? 1 : 0);
  return ok;
}

void VoiceScreen::endCall(UITask& ui, bool send_end) {
  if (g_call_state == CALL_IDLE && !g_ptt && !g_incoming) {
    // Still tear down any leftover workers (safe re-entry)
    stopVoiceWorker();
    stopDecWorker();
    freeHdStack();
    spk_i2s_stop();
    return;
  }

  if (send_end && g_has_target &&
      (g_call_state == CALL_CONNECTED || g_call_state == CALL_OUTGOING)) {
    sendVoiceControl(ui, g_target, VOICE_CTRL_END);
  }

  g_ptt = false;
  g_tx_finishing = false;
  g_flush_req = false;
  g_call_state = CALL_IDLE;
  g_incoming = false;
  g_incoming_name[0] = 0;
  g_has_caller = false;
  g_accept_retx = false;
  g_accept_retx_count = 0;
  g_accept_last_ms = 0;
  g_packets_sent = g_packets_acked = g_packets_lost = 0;
  g_tx_seq = 0;
  g_voice_ack_idx = 0;
  g_last_rx_play_ms = 0;
  g_call_connected_ms = 0;
  memset(g_voice_acks, 0, sizeof(g_voice_acks));
  g_pending_len = 0;
  g_pending_eos = false;
  _auto_invite = false;
  jitterReset();
  // Drain RX queue
  g_rx_head = g_rx_tail = g_rx_count = 0;
  stopVoiceWorker();
  stopDecWorker();
  freeHdStack();
  stopRxWorker();
  if (g_mic_started) {
    mic_i2s_stop();
    g_mic_started = false;
  }
  spk_i2s_stop();
  if (g_c2.ready()) g_c2.clearPacket();
  strcpy(_status, "Call ended");
  Serial.println("[voice] call ended (cleanup done)");
}

// Private helpers (beta block only — non-beta stubs after #endif below)
bool VoiceScreen::startCodec2Init(const char* status_while) {
  if (g_c2.ready()) return true;
  if (g_c2_init_req) return true; // already spinning

  g_c2_init_req  = true;
  g_c2_init_done = false;
  g_c2_init_ok   = false;
  voiceMem("startCodec2Init: before c2init stack");
  // codec2_create is stack-heavy (FFT tables / mode setup); 12 KB can overflow.
  const uint32_t stack_bytes = 16384;
  if (!g_c2_stack)
    g_c2_stack = allocInternalStack(stack_bytes, "c2init");
  if (!g_c2_stack) {
    strcpy(_status, "No internal stack");
    g_c2_init_req = false;
    voiceMem("startCodec2Init: FAILED c2init stack");
    Serial.println(
        "[voice] DIAG: cannot alloc 16 KB INTERNAL for codec2_create task.\n"
        "  Codec2 object can live in PSRAM; the *init task stack* cannot.");
    return false;
  }
  g_c2_task = xTaskCreateStaticPinnedToCore(
      codec2InitTask, "c2init",
      stack_bytes / sizeof(StackType_t),
      nullptr, 1, g_c2_stack, &g_c2_task_buf, 1);
  if (!g_c2_task) {
    strcpy(_status, "Init task failed");
    g_c2_init_req = false;
    return false;
  }
  if (status_while) {
    strncpy(_status, status_while, sizeof(_status) - 1);
    _status[sizeof(_status) - 1] = 0;
  }
  return true;
}

void VoiceScreen::onConnectedMedia() {
  // Order matters for INTERNAL RAM:
  //  1) finish codec2 init (temp stack, free only after task exits)
  //  2) grab large HD stack once (c2dec XOR c2work for whole call)
  //  3) small c2rx last so it cannot fragment the big block
  // Do NOT free/alloc other stacks while c2init is running.
  g_call_connected_ms = millis();
  g_tx_finishing = false;
  g_ptt = false;
  if (!g_c2.ready()) {
    startCodec2Init("Preparing audio...");
    // tick1s: free c2init stack → HD → c2rx after init completes
    return;
  }
  freeCodec2InitStack();  // safe: no live c2init task
  if (!ensureHdStack()) {
    Serial.println("[voice] onConnected: HD stack delayed — will retry on media");
  }
  ensureRxWorker();
  strcpy(_status, "Listening - ENTER to talk");
}

void VoiceScreen::handleCallControl(UITask& ui, uint8_t ctrl,
                                    const char* from_name, float snr,
                                    const ContactInfo* from) {
  Serial.printf("[voice] CTRL RX %u from=%s state=%d\n",
                ctrl, from_name ? from_name : "?", (int)g_call_state);

  switch (ctrl) {
  case VOICE_CTRL_INVITE:
    // Already busy with a call — BUSY the inviter (not our peer target)
    if (g_call_state == CALL_CONNECTED || g_call_state == CALL_OUTGOING ||
        g_call_state == CALL_INCOMING) {
      if (from)
        sendVoiceControl(ui, *from, VOICE_CTRL_BUSY);
      return;
    }
    g_call_state = CALL_INCOMING;
    g_incoming = true;
    g_incoming_snr = snr;
    strncpy(g_incoming_name, from_name ? from_name : "?", sizeof(g_incoming_name) - 1);
    g_incoming_name[sizeof(g_incoming_name) - 1] = 0;
    g_call_start_ms = millis();
    if (from) {
      g_caller = *from;
      g_has_caller = true;
      g_target = *from;
      g_has_target = true;
    } else {
      g_has_caller = false;
    }
    snprintf(_status, sizeof(_status), "Incoming: %s", g_incoming_name);
    if (!ui.hw.isDisplayOn()) ui.hw.displayOn();
    ui.hw.kickActivity();
    ui.hw.beep(1400, 80);
    // Stay on current screen — UITask draws global Accept/Decline overlay
    ui.requestDraw();
    break;

  case VOICE_CTRL_ACCEPT:
    // Idempotent: OUTGOING → CONNECTED; ignore if already connected
    Serial.printf("[voice] ACCEPT rx state=%d from=%s\n",
                  (int)g_call_state, from_name ? from_name : "?");
    if (g_call_state == CALL_OUTGOING) {
      g_call_state = CALL_CONNECTED;
      g_incoming = false;
      g_accept_retx = false;
      g_call_connected_ms = millis();
      g_packets_sent = g_packets_acked = g_packets_lost = 0;
      g_tx_seq = 0;
      memset(g_voice_acks, 0, sizeof(g_voice_acks));
      if (from) {
        g_target = *from;
        g_has_target = true;
      }
      onConnectedMedia();
      ui.hw.beep(1800, 40);
      strcpy(_status, "Listening - ENTER to talk");
      ui.requestDraw();
      Serial.println("[voice] remote ACCEPTED – media enabled");
    } else if (g_call_state == CALL_CONNECTED) {
      // Peer retransmitting ACCEPT (or we already connected) — stop our retx
      g_accept_retx = false;
      ui.requestDraw();
    } else {
      Serial.printf("[voice] ACCEPT ignored (state=%d)\n", (int)g_call_state);
    }
    break;

  case VOICE_CTRL_DECLINE:
  case VOICE_CTRL_BUSY:
  case VOICE_CTRL_END:
    if (g_call_state != CALL_IDLE) {
      endCall(ui, false);
      if (ctrl == VOICE_CTRL_BUSY)
        strcpy(_status, "Busy");
      else if (ctrl == VOICE_CTRL_DECLINE)
        strcpy(_status, "Declined");
      else
        strcpy(_status, "Remote ended");
      ui.requestDraw();
    }
    break;
  }
}

#endif // MESHDECK_BETA

#ifndef MESHDECK_BETA
// Stubs so stable builds link when UITask / Contacts reference the API
bool VoiceScreen::startCodec2Init(const char* status_while) {
  (void)status_while; return false;
}
void VoiceScreen::onConnectedMedia() {}
void VoiceScreen::onPacketSent(uint32_t tag, uint16_t len, bool eos) {
  (void)tag; (void)len; (void)eos;
}
void VoiceScreen::onPacketAcked(uint32_t tag, bool eos) {
  (void)tag; (void)eos;
}
void VoiceScreen::checkVoiceAcks() {}
#endif

void VoiceScreen::enter() {
#ifdef MESHDECK_BETA
  g_ptt = false;
  g_tx_finishing = false;
  _hwok = es7210_probe();

  if (g_has_target && ui.mesh) {
    ContactInfo c;
    if (ui.mesh->getContactByIdx((uint32_t)g_contact_idx, c) &&
        memcmp(c.id.pub_key, g_target.id.pub_key, 6) == 0) {
      g_target = c;
    }
  }

  if (_auto_invite && g_call_state == CALL_IDLE && g_has_target) {
    _auto_invite = false;
    beginOutboundInvite();
  } else if (g_call_state == CALL_INCOMING) {
    snprintf(_status, sizeof(_status), "Incoming: %s", g_incoming_name);
  } else if (g_call_state == CALL_CONNECTED) {
    if (g_c2.ready())
      strcpy(_status, "Listening - ENTER to talk");
    else
      strcpy(_status, "Preparing audio...");
  } else if (g_call_state == CALL_OUTGOING) {
    snprintf(_status, sizeof(_status), "Calling %s...", g_target.name);
  } else if (!g_has_target) {
    strcpy(_status, "Start from Contacts > Call");
  } else if (!_hwok) {
    snprintf(_status, sizeof(_status), "To: %s (no mic)", g_target.name);
  } else {
    snprintf(_status, sizeof(_status), "Ready - ENTER to redial");
  }

  Serial.printf("[voice] enter hwok=%d target=%s state=%d ready=%d auto=%d vol=%d\n",
                _hwok ? 1 : 0,
                g_has_target ? g_target.name : "(none)",
                (int)g_call_state,
                g_c2.ready() ? 1 : 0,
                _auto_invite ? 1 : 0,
                g_call_vol);
#else
  strcpy(_status, "beta build only");
#endif
}

void VoiceScreen::leave() {
#ifdef MESHDECK_BETA
  // Leaving the call UI always hangs up so the next call starts clean.
  Serial.printf("[voice] leave state=%d ptt=%d\n",
                (int)g_call_state, g_ptt ? 1 : 0);
  if (g_call_state != CALL_IDLE || g_incoming || g_ptt) {
    endCall(ui, true);
  } else {
    stopVoiceWorker();
    stopDecWorker();
    freeHdStack();
    stopRxWorker();
    spk_i2s_stop();
  }
  _auto_invite = false;
#else
  (void)0;
#endif
}

void VoiceScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Voice Call");

#ifndef MESHDECK_BETA
  c.setTextColor(C_YELLOW);
  c.setCursor(12, 70);
  c.print("Experimental - flash the beta");
  c.setCursor(12, 86);
  c.print("build to enable voice calls.");
  return;
#else
  if (g_call_state == CALL_INCOMING || g_incoming) {
    drawIncomingOverlay(c);
    return;
  }

  const int left = 12;
  int y = STATUS_H + 6;

  // Peer name
  c.setTextColor(C_FG_DIM);
  c.setCursor(left, y);
  c.print("With");
  c.setTextSize(2);
  c.setTextColor(g_has_target ? C_ACCENT : C_RED);
  c.setCursor(left, y + 14);
  if (g_has_target) {
    char name[18];
    ellipsize(name, sizeof(name), g_target.name);
    c.print(name);
  } else {
    c.print("(none)");
  }
  c.setTextSize(1);
  y += 36;

  // Big half-duplex state
  c.setTextSize(2);
  if (g_call_state == CALL_OUTGOING) {
    c.setTextColor(C_YELLOW);
    c.setCursor(left, y);
    c.print("RINGING");
  } else if (g_ptt) {
    c.setTextColor(C_RED);
    c.setCursor(left, y);
    c.print("TALKING");
  } else if (g_tx_finishing) {
    c.setTextColor(C_ORANGE);
    c.setCursor(left, y);
    c.print("SENDING");
  } else if (peerRecentlyTalking()) {
    // Do not use g_spk_started — I2S can stay open briefly; HEARING is play-window only
    c.setTextColor(C_GREEN);
    c.setCursor(left, y);
    c.print("HEARING");
  } else if (g_call_state == CALL_CONNECTED) {
    c.setTextColor(C_GREEN);
    c.setCursor(left, y);
    c.print("LISTEN");
  } else {
    c.setTextColor(C_FG);
    c.setCursor(left, y);
    c.print("IDLE");
  }
  c.setTextSize(1);
  y += 22;

  c.setTextColor(C_FG);
  c.setCursor(left, y);
  {
    char st[sizeof(_status)];
    ellipsize(st, sizeof(st), _status);  // ASCII-only + truncate for GFX font
    c.print(st);
  }
  y += 16;

  // Volume bar (RX)
  if (g_call_state == CALL_CONNECTED || g_call_state == CALL_OUTGOING) {
    c.setTextColor(C_FG_DIM);
    c.setCursor(left, y);
    c.print("Vol");
    int bar_x = 48;
    int bar_w = SCREEN_W - bar_x - 40;
    c.drawRect(bar_x, y - 1, bar_w, 12, C_FG_FAINT);
    int fill = (bar_w - 2) * g_call_vol / 10;
    if (fill > 0)
      c.fillRect(bar_x + 1, y, fill, 10, C_ACCENT);
    c.setTextColor(C_FG);
    c.setCursor(bar_x + bar_w + 6, y);
    c.printf("%d", g_call_vol);
    y += 16;
  }

  // Call timer + stats
  if (g_call_state == CALL_CONNECTED && g_call_connected_ms) {
    uint32_t sec = (millis() - g_call_connected_ms) / 1000;
    c.setTextColor(C_FG_DIM);
    c.setCursor(left, y);
    c.print("Call");
    c.setTextColor(C_FG);
    c.setCursor(70, y);
    c.printf("%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    y += 13;
  }
  if (g_ptt && g_ptt_start_ms) {
    uint32_t sec = (millis() - g_ptt_start_ms) / 1000;
    c.setTextColor(C_FG_DIM);
    c.setCursor(left, y);
    c.print("PTT");
    c.setTextColor(C_RED);
    c.setCursor(70, y);
    c.printf("%lu s  (release ENTER)", (unsigned long)sec);
    y += 13;
  }
  if (g_packets_sent > 0) {
    c.setTextColor(C_FG_DIM);
    c.setCursor(left, y);
    c.print("Pkts");
    c.setTextColor(g_packets_lost ? C_YELLOW : C_GREEN);
    c.setCursor(70, y);
    c.printf("%d ok / %d sent", g_packets_acked, g_packets_sent);
    if (g_packets_lost) c.printf("  lost %d", g_packets_lost);
    y += 13;
  }

  c.setTextColor(C_FG_DIM);
  c.setCursor(left, y);
  c.print("Mode");
  c.setTextColor(C_FG);
  c.setCursor(70, y);
  c.print("Half-duplex PTT  Codec2 1200");
  y += 13;

  // Help footer
  const int helpY = SCREEN_H - 48;
  c.drawFastHLine(0, helpY - 4, SCREEN_W, C_FG_FAINT);
  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, helpY);
  if (g_call_state == CALL_CONNECTED) {
    if (g_ptt)
      c.print("ENTER  stop talking  (then waits to send)");
    else if (g_tx_finishing)
      c.print("Sending buffered speech over LoRa...");
    else
      c.print("ENTER  talk   +/-  volume");
  } else if (g_call_state == CALL_OUTGOING) {
    c.print("ENTER or BACK  cancel ring");
  } else {
    c.print("Contacts > Call... to dial");
  }
  c.setCursor(6, helpY + 12);
  c.print("BACK  hang up & leave");
  c.setCursor(6, helpY + 24);
  c.print("One talks at a time ~1s LoRa bursts");
#endif
}

bool VoiceScreen::key(uint8_t k) {
#ifdef MESHDECK_BETA
  // Decline (also handled globally by UITask overlay)
  if ((g_call_state == CALL_INCOMING || g_incoming) &&
      (k == 'n' || k == 'N' || k == 0x1B)) {
    rejectInbound(true);
    return true;
  }

  // Accept (also handled globally by UITask overlay)
  if ((g_call_state == CALL_INCOMING || g_incoming) &&
      (k == 0x0D || k == ' ')) {
    acceptInbound();
    return true;
  }

  // RX volume anytime on call screen
  if (k == '+' || k == '=' || k == ']') {
    if (g_call_vol < 10) g_call_vol++;
    snprintf(_status, sizeof(_status), "Volume %d/10", g_call_vol);
    ui.requestDraw();
    return true;
  }
  if (k == '-' || k == '_' || k == '[') {
    if (g_call_vol > 1) g_call_vol--;
    snprintf(_status, sizeof(_status), "Volume %d/10", g_call_vol);
    ui.requestDraw();
    return true;
  }

  // ESC / N hang up (stay on screen with status — BACK leaves)
  if ((k == 0x1B || k == 'n' || k == 'N') &&
      (g_call_state == CALL_OUTGOING || g_call_state == CALL_CONNECTED)) {
    if (g_ptt) {
      g_ptt = false;
      g_tx_finishing = true;
      g_flush_req = true;
    }
    endCall(ui, true);
    strcpy(_status, "Call ended");
    ui.requestDraw();
    return true;
  }

  // Main ENTER / Space / T — PTT toggle
  if (k == 0x0D || k == ' ' || k == 't' || k == 'T') {
    if (g_call_state == CALL_INCOMING) return true;

    if (g_call_state == CALL_OUTGOING) {
      endCall(ui, true);
      strcpy(_status, "Call cancelled");
      ui.requestDraw();
      return true;
    }

    if (g_call_state == CALL_IDLE) {
      if (!g_has_target) {
        strcpy(_status, "Start from Contacts > Call");
        ui.requestDraw();
        return true;
      }
      if (!beginOutboundInvite()) {
        ui.requestDraw();
        return true;
      }
      ui.requestDraw();
      return true;
    }

    if (g_call_state == CALL_CONNECTED) {
      if (!g_c2.ready()) {
        startCodec2Init("Preparing audio...");
        ui.requestDraw();
        return true;
      }
      if (!g_ptt) {
        // Wait for previous burst to finish sending
        if (g_tx_finishing || g_pending_len > 0 || countOpenVoiceAcks() > 0) {
          strcpy(_status, "Wait - still sending...");
          ui.requestDraw();
          return true;
        }
        if (!_hwok) {
          strcpy(_status, "No mic - RX only");
          ui.requestDraw();
          return true;
        }
        if (!g_frame) {
          g_frame = (int16_t*)heap_caps_malloc(
              FRAME_SAMPLES_16K * sizeof(int16_t),
              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
          if (!g_frame) {
            strcpy(_status, "No audio buffer");
            ui.requestDraw();
            return true;
          }
        }

        // Half-duplex: seize floor — stop speaker/decode immediately
        stopDecWorker();
        spk_i2s_stop();
        g_last_rx_play_ms = 0;

        g_c2.clearPacket();
        g_pending_len  = 0;
        g_pending_eos  = false;
        g_flush_req    = false;
        g_tx_finishing = false;
        g_ptt_start_ms = millis();
        g_ptt = true;

        if (!ensureVoiceWorker()) {
          g_ptt = false;
          strcpy(_status, "Voice worker failed");
          ui.requestDraw();
          return true;
        }
        strcpy(_status, "Talking - ENTER when done");
        Serial.println("[voice] PTT ON (half-duplex floor)");
        ui.hw.beep(1400, 25);
      } else {
        // Release floor: stop capture, flush remaining frames as EOS packet
        Serial.println("[voice] PTT OFF → buffer & send");
        g_ptt = false;
        g_tx_finishing = true;
        g_flush_req = true;
        strcpy(_status, "Sending...");
      }
      ui.requestDraw();
      return true;
    }
  }
#endif
  return false;
}

bool VoiceScreen::nav(NavEvent e) {
#ifdef MESHDECK_BETA
  if (e == NAV_SELECT) {
    return key(0x0D);
  }
  if (e == NAV_UP) {
    if (g_call_vol < 10) g_call_vol++;
    snprintf(_status, sizeof(_status), "Volume %d/10", g_call_vol);
    ui.requestDraw();
    return true;
  }
  if (e == NAV_DOWN) {
    if (g_call_vol > 1) g_call_vol--;
    snprintf(_status, sizeof(_status), "Volume %d/10", g_call_vol);
    ui.requestDraw();
    return true;
  }
  if (e == NAV_BACK) {
    // Hang up and leave screen (leave() also cleans up if go/back path)
    if (g_ptt) {
      g_ptt = false;
      g_tx_finishing = false;
      g_flush_req = false;
    }
    if (g_call_state != CALL_IDLE || g_incoming)
      endCall(ui, true);
    return false;  // let UITask::back() run (leave + previous screen)
  }
#endif
  return false;
}

void VoiceScreen::tick1s() {
#ifdef MESHDECK_BETA
  // Wait until c2init task has fully exited (handle cleared) before freeing stack
  if (g_c2_init_req && g_c2_init_done && !g_c2_task) {
    freeCodec2InitStack();
    g_c2_init_req = false;

    if (g_c2_init_ok && g_c2.ready()) {
      // Prefer HD stack while free blocks are still large, then jitter task
      if (g_call_state == CALL_CONNECTED)
        ensureHdStack();
      ensureRxWorker();  // may already be running
      if (g_call_state == CALL_CONNECTED) {
        // Lazy c2dec: only if listening (not PTT) and jitter already has data
        if (!g_ptt && jitterCount() > 0)
          ensureDecWorker();
        strcpy(_status, "Listening - ENTER to talk");
      } else if (g_call_state == CALL_OUTGOING) {
        // keep ringing status
      } else {
        strcpy(_status, "Ready");
      }
      ui.requestDraw();
    } else {
      strcpy(_status, "Codec2 init failed");
      ui.requestDraw();
    }
  }

  // Ring timeout
  if (g_call_state == CALL_OUTGOING &&
      (millis() - g_call_start_ms) > CALL_RING_TIMEOUT_MS) {
    Serial.println("[voice] ring timeout");
    endCall(ui, true);
    strcpy(_status, "No answer");
    ui.requestDraw();
  }
  if (g_call_state == CALL_INCOMING &&
      (millis() - g_call_start_ms) > CALL_RING_TIMEOUT_MS) {
    // Missed: clear overlay, no reject DM (saves airtime)
    Serial.println("[voice] incoming ring timeout");
    endCall(ui, false);
    strcpy(_status, "Missed call");
    ui.toast("Missed voice call", C_YELLOW);
    ui.requestDraw();
  }

  // Callee: retransmit ACCEPT until peer is clearly up (media heard) or max tries.
  // Stop once we are listening and codec is ready + a few seconds elapsed.
  if (g_accept_retx && g_call_state == CALL_CONNECTED &&
      g_call_connected_ms &&
      (millis() - g_call_connected_ms) > 8000) {
    g_accept_retx = false;
  }
  if (g_accept_retx && g_call_state == CALL_CONNECTED && g_has_target &&
      g_accept_retx_count < ACCEPT_RETX_MAX &&
      (millis() - g_accept_last_ms) >= ACCEPT_RETX_MS) {
    g_accept_last_ms = millis();
    g_accept_retx_count++;
    Serial.printf("[voice] ACCEPT retx %u/%u -> %s\n",
                  (unsigned)g_accept_retx_count, (unsigned)ACCEPT_RETX_MAX,
                  g_target.name);
    sendVoiceControl(ui, g_target, VOICE_CTRL_ACCEPT);
    if (g_accept_retx_count >= ACCEPT_RETX_MAX) {
      g_accept_retx = false;
      Serial.println("[voice] ACCEPT retx exhausted");
    }
  }

  checkVoiceAcks();

  // Only arm "missed" if a talk burst fully failed (0 acks) — not after good calls
  if (!g_ptt && !g_tx_finishing && g_pending_len == 0 &&
      g_packets_sent > 0 && g_packets_acked == 0 && !g_missed_pending &&
      g_call_state == CALL_CONNECTED) {
    bool any_open = false;
    for (int i = 0; i < VOICE_ACK_SLOTS; i++) {
      if (g_voice_acks[i].tag && !g_voice_acks[i].acked && !g_voice_acks[i].timed_out)
        any_open = true;
    }
    if (!any_open)
      missedCallArmFromSession();
  }

  if (g_missed_pending &&
      g_missed_sends < MISSED_CALL_MAX_SENDS &&
      (g_missed_last_try == 0 ||
       millis() - g_missed_last_try >= MISSED_CALL_RETRY_MS)) {
    missedCallSendOnce(ui);
    ui.requestDraw();
  }
#endif
}

// ── Call lifecycle (Contacts → Call... / global ring UI) ──
void VoiceScreen::prepareOutbound(const ContactInfo& to) {
#ifdef MESHDECK_BETA
  g_target = to;
  g_has_target = true;
  g_contact_idx = 0;
  if (ui.mesh) {
    for (int i = 0; i < ui.mesh->getNumContacts(); i++) {
      ContactInfo c;
      if (ui.mesh->getContactByIdx(i, c) &&
          memcmp(c.id.pub_key, to.id.pub_key, 6) == 0) {
        g_target = c;
        g_contact_idx = i;
        break;
      }
    }
  }
  _auto_invite = true;
  snprintf(_status, sizeof(_status), "Calling %s...", g_target.name);
#else
  (void)to;
#endif
}

bool VoiceScreen::beginOutboundInvite() {
#ifdef MESHDECK_BETA
  if (g_call_state != CALL_IDLE) return false;
  if (!g_has_target) {
    strcpy(_status, "No contact");
    return false;
  }
  // Refresh path; refuse multi-hop mesh (voice max 1 hop)
  if (ui.mesh) {
    if (ContactInfo* live =
            ui.mesh->lookupContactByPubKey(g_target.id.pub_key, 6)) {
      g_target = *live;
    }
    if (!ui.mesh->canVoiceCallContact(g_target)) {
      uint8_t hops = MyMesh::voiceHopCount(g_target.out_path_len);
      snprintf(_status, sizeof(_status), "Too far (%u hops, max %u)",
               (unsigned)hops, (unsigned)MyMesh::VOICE_MAX_HOPS);
      ui.toast("Call limited to 1 hop", C_YELLOW);
      return false;
    }
  }
  // INVITE is 2 bytes – no Codec2 / workers / DMA yet
  if (!sendVoiceControl(ui, g_target, VOICE_CTRL_INVITE)) {
    strcpy(_status, "Invite failed");
    return false;
  }
  g_call_state = CALL_OUTGOING;
  g_call_start_ms = millis();
  g_packets_sent = g_packets_acked = g_packets_lost = 0;
  memset(g_voice_acks, 0, sizeof(g_voice_acks));
  snprintf(_status, sizeof(_status), "Calling %s...", g_target.name);
  ui.hw.beep(1200, 30);
  Serial.println("[voice] INVITE sent – waiting for ACCEPT");
  return true;
#else
  strcpy(_status, "beta build only");
  return false;
#endif
}

void VoiceScreen::acceptInbound() {
#ifdef MESHDECK_BETA
  if (g_call_state != CALL_INCOMING && !g_incoming) return;

  if (g_has_caller) {
    g_target = g_caller;
    g_has_target = true;
  } else if (!g_has_target && ui.mesh && g_incoming_name[0]) {
    for (int i = 0; i < ui.mesh->getNumContacts(); i++) {
      ContactInfo c;
      if (ui.mesh->getContactByIdx(i, c) &&
          strncmp(c.name, g_incoming_name, 31) == 0) {
        g_target = c;
        g_has_target = true;
        g_contact_idx = i;
        break;
      }
    }
  }

  g_incoming = false;
  if (!g_has_target) {
    Serial.println("[voice] ACCEPT not sent — no target contact!");
    strcpy(_status, "Accept failed (no contact)");
    ui.requestDraw();
    return;
  }

  // Refresh live path/secret before signaling (snapshot from INVITE can be stale)
  if (ui.mesh) {
    if (ContactInfo* live =
            ui.mesh->lookupContactByPubKey(g_target.id.pub_key, 6)) {
      g_target = *live;
    }
  }

  // Send ACCEPT, space on air, then again. Do NOT start Codec2 yet — heavy
  // init can starve the radio TX queue right when the caller needs this packet.
  bool ok1 = sendVoiceControl(ui, g_target, VOICE_CTRL_ACCEPT);
  vTaskDelay(pdMS_TO_TICKS(200));
  bool ok2 = sendVoiceControl(ui, g_target, VOICE_CTRL_ACCEPT);
  Serial.printf("[voice] ACCEPT TX to %s ok=%d/%d\n", g_target.name, ok1 ? 1 : 0, ok2 ? 1 : 0);

  g_call_state = CALL_CONNECTED;
  g_call_start_ms = millis();
  g_call_connected_ms = millis();
  g_packets_sent = g_packets_acked = g_packets_lost = 0;
  g_tx_seq = 0;
  memset(g_voice_acks, 0, sizeof(g_voice_acks));
  g_accept_retx = true;
  g_accept_retx_count = 0;
  g_accept_last_ms = millis();

  // Let mesh TX drain before codec2 stacks / workers compete for RAM/CPU
  vTaskDelay(pdMS_TO_TICKS(100));
  onConnectedMedia();
  ui.hw.beep(1800, 40);
  Serial.printf("[voice] ACCEPT -> CONNECTED with %s\n", g_target.name);
  ui.requestDraw();
#else
  (void)0;
#endif
}

void VoiceScreen::rejectInbound(bool send_dm) {
#ifdef MESHDECK_BETA
  const ContactInfo* who = nullptr;
  if (g_has_caller) who = &g_caller;
  else if (g_has_target) who = &g_target;

  if (who) {
    sendVoiceControl(ui, *who, VOICE_CTRL_DECLINE);
    if (send_dm)
      ui.sendDM(who->id.pub_key, REJECT_DM_TEXT);
  }
  endCall(ui, false);
  strcpy(_status, "Declined");
  ui.requestDraw();
#else
  (void)send_dm;
#endif
}

bool VoiceScreen::hasIncomingCall() const {
#ifdef MESHDECK_BETA
  return g_call_state == CALL_INCOMING || g_incoming;
#else
  return false;
#endif
}

bool VoiceScreen::isInCall() const {
#ifdef MESHDECK_BETA
  return g_call_state != CALL_IDLE;
#else
  return false;
#endif
}

void VoiceScreen::drawIncomingOverlay(GFXcanvas16& c) {
#ifdef MESHDECK_BETA
  const int mw = SCREEN_W - 24;
  const int mh = 120;
  const int mx = 12;
  const int my = (SCREEN_H - mh) / 2;

  // dim full screen
  c.fillRect(0, 0, SCREEN_W, SCREEN_H, (uint16_t)((C_BG >> 1) & 0x7BEF));

  c.fillRoundRect(mx, my, mw, mh, 10, C_BG_RAISED);
  c.drawRoundRect(mx, my, mw, mh, 10, C_GREEN);

  c.setTextSize(1);
  c.setTextColor(C_GREEN);
  c.setCursor(mx + 14, my + 12);
  c.print("Incoming call");

  c.setTextSize(2);
  c.setTextColor(C_FG);
  c.setCursor(mx + 14, my + 36);
  {
    char lab[22];
    ellipsize(lab, sizeof(lab), g_incoming_name[0] ? g_incoming_name
                       : (g_has_caller ? g_caller.name : "?"));
    c.print(lab);
  }

  c.setTextSize(1);
  c.setTextColor(C_FG_DIM);
  c.setCursor(mx + 14, my + 68);
  c.printf("SNR %.1f dB", g_incoming_snr);

  c.setTextColor(C_ACCENT);
  c.setCursor(mx + 14, my + 88);
  c.print("ENTER Accept   N Decline");
#else
  (void)c;
#endif
}

void VoiceScreen::pushRxVoice(const uint8_t* data, size_t len, bool eos,
                              const char* from_name, float snr,
                              const ContactInfo* from) {
#ifdef MESHDECK_BETA
  if (!data || len == 0 || len > RX_PKT_MAX) return;

  const char* who = (from_name && from_name[0]) ? from_name
                    : (from ? from->name : "?");

  // Control packets are handled immediately (no jitter)
  if (len >= 2 && (data[0] & VOICE_FLAG_CTRL)) {
    uint8_t ctrl = data[0] & 0x0F;
    handleCallControl(ui, ctrl, who, snr, from);
    return;
  }

  // Media while we are still RINGING: peer accepted / is talking but ACCEPT
  // control was lost — complete the handshake so we leave RING.
  if (g_call_state == CALL_OUTGOING) {
    Serial.println("[voice] media while OUTGOING — treating as ACCEPT");
    handleCallControl(ui, VOICE_CTRL_ACCEPT, who, snr, from);
  }

  // Media packets only accepted while CONNECTED
  if (g_call_state != CALL_CONNECTED) {
    // Treat first media packet while idle as an implicit INVITE
    // (backward compatibility with pure-PTT peers)
    if (g_call_state == CALL_IDLE && !g_ptt) {
      handleCallControl(ui, VOICE_CTRL_INVITE, who, snr, from);
    }
    // Still buffer nothing until accepted
    return;
  }

  // Hearing peer media means the handshake completed both ways — stop ACCEPT retx
  g_accept_retx = false;

  if (!ensureRxWorker()) {
    Serial.printf("[voice] RX worker unavailable – drop len=%u from=%s\n",
                  (unsigned)len, who);
    return;
  }

  if (!rx_enqueue(data, len, eos, true, who)) {
    Serial.printf("[voice] RX queue full – drop len=%u from=%s\n",
                  (unsigned)len, who);
    return;
  }

  // First inbound media while listening: start decode worker (lazy, half-duplex)
  if (!g_ptt && !g_tx_finishing && g_c2.ready())
    ensureDecWorker();

  Serial.printf("[voice] RX queued len=%u eos=%d from=%s snr=%.1f depth=%d\n",
                (unsigned)len, eos ? 1 : 0, who, snr, g_rx_count);

  static uint32_t last_draw_ms = 0;
  if (eos || millis() - last_draw_ms > 300) {
    last_draw_ms = millis();
    if (!g_ptt) {
      snprintf(_status, sizeof(_status), "RX %s%s", who, eos ? " (end)" : "");
    }
    ui.requestDraw();
  }
#else
  (void)data; (void)len; (void)eos; (void)from_name; (void)snr; (void)from;
#endif
}

#ifdef MESHDECK_BETA

void VoiceScreen::onPacketAcked(uint32_t tag, bool eos) {
  if (tag == 0) return;

  for (int i = 0; i < VOICE_ACK_SLOTS; i++) {
    VoiceAckSlot& s = g_voice_acks[i];
    if (s.tag != tag) continue;

    // Late ACK after we already timed out — still count delivery
    if (s.acked) return;
    if (s.timed_out) {
      s.timed_out = false;
      if (g_packets_lost > 0) g_packets_lost--;
    }
    s.acked = true;
    g_packets_acked++;
    uint32_t rtt = millis() - s.sent_ms;
    Serial.printf("[voice] ACK tag=%u seq=%u rtt=%u ms eos=%d acked=%d lost=%d\n",
                  tag, s.seq, rtt, eos ? 1 : 0,
                  g_packets_acked, g_packets_lost);
    ui.requestDraw();
    if (g_missed_pending && g_has_target &&
        memcmp(g_missed_prefix, g_target.id.pub_key, 6) == 0) {
      missedCallClear();
    }
    return;
  }
  // Control INVITE/ACCEPT also get mesh ACKs but never register slots — quiet.
  Serial.printf("[voice] ACK untracked tag=%u (ctrl or recycled slot)\n", tag);
}

void VoiceScreen::checkVoiceAcks() {
  uint32_t now = millis();
  bool changed = false;

  for (int i = 0; i < VOICE_ACK_SLOTS; i++) {
    VoiceAckSlot& s = g_voice_acks[i];
    if (s.tag == 0 || s.acked || s.timed_out) continue;
    if (now - s.sent_ms < VOICE_ACK_TIMEOUT_MS) continue;

    // Defer RETX while user still holds PTT (keeps air free for original TX + ACK)
    if (g_ptt && s.retx < VOICE_MAX_RETX) {
      s.sent_ms = now;  // extend wait until PTT ends
      continue;
    }

    // One retx — MeshCore assigns a *new* tag; must store it or ACK never matches
    if (s.retx < VOICE_MAX_RETX && ui.mesh && g_has_target) {
      s.data[0] |= VOICE_FLAG_RETX;
      s.retx++;
      uint32_t new_tag = 0;
      bool ok = ui.mesh->sendVoiceToContact(g_target, s.data, s.len, s.eos,
                                            true, &new_tag);
      if (ok && new_tag) {
        s.tag = new_tag;
        s.sent_ms = now;
      }
      Serial.printf("[voice] RETX seq=%u tag=%u eos=%d ok=%d\n",
                    s.seq, s.tag, s.eos ? 1 : 0, ok ? 1 : 0);
      continue;
    }

    s.timed_out = true;
    g_packets_lost++;
    Serial.printf("[voice] TIMEOUT seq=%u tag=%u acked=%d lost=%d\n",
                  s.seq, s.tag, g_packets_acked, g_packets_lost);
    changed = true;
  }

  if (changed) {
    snprintf(_status, sizeof(_status), "Lost %d / sent %d", g_packets_lost, g_packets_sent);
    ui.requestDraw();
  }
}

void VoiceScreen::pollPTT() {
  if (!ui.mesh || !g_has_target) return;
  if (g_call_state != CALL_CONNECTED) return;

  int n = g_pending_len;
  if (n <= 0) {
    // Finished flushing: return to listen mode (half-duplex)
    if (g_tx_finishing && !g_ptt && countOpenVoiceAcks() == 0) {
      g_tx_finishing = false;
      stopVoiceWorker();  // stop TX task; HD stack reused by c2dec
      freeCodec2InitStack();
      strcpy(_status, "Listening - ENTER to talk");
      // Prefer starting decode promptly if peer already sent
      if (jitterCount() > 0 && g_c2.ready())
        ensureDecWorker();
      ui.requestDraw();
    }
    return;
  }

  // Stop-and-wait: 1 media datagram in flight (ACK needs quiet RX)
  if (countOpenVoiceAcks() >= VOICE_MAX_INFLIGHT)
    return;

  bool eos = g_pending_eos;

  // Codec payload cap: req = 4 + n must be ≤ 168 → n ≤ 164 (see Codec2Engine)
  uint8_t framed[182];
  if (n > 162) n = 162;  // 27×6 max theoretical; engine uses 24×6=144
  framed[0] = eos ? VOICE_FLAG_EOS : 0x00;
  framed[1] = g_tx_seq++;
  memcpy(framed + 2, g_pending_pkt, n);
  uint16_t framed_len = (uint16_t)(2 + n);

  bool ok = ui.mesh->sendVoiceToContact(g_target, framed, framed_len, eos);
  static uint8_t fail_streak = 0;

  if (ok) {
    fail_streak = 0;
    g_packets_sent++;
    int idx = (g_voice_ack_idx + VOICE_ACK_SLOTS - 1) % VOICE_ACK_SLOTS;
    VoiceAckSlot& s = g_voice_acks[idx];
    s.seq = framed[1];
    s.len = framed_len;
    s.eos = eos;
    s.retx = 0;
    memcpy(s.data, framed, framed_len);

    Serial.printf("[voice] packet #%d %u B eos=%d seq=%u inflight=%d\n",
                  g_packets_sent, (unsigned)n, eos ? 1 : 0, framed[1],
                  countOpenVoiceAcks());
    g_pending_len = 0;

    if (eos) {
      // Floor released: stop TX task; shared HD stack ready for peer decode
      stopVoiceWorker();
      g_ptt = false;
      g_tx_finishing = true;
      strcpy(_status, "Sent - listening...");
      ui.requestDraw();
    }
  } else {
    Serial.println("[voice] sendVoiceToContact failed");
    g_tx_seq--;
    // Avoid infinite "SENDING" if packet is still too big or radio rejects
    if (++fail_streak >= 5) {
      fail_streak = 0;
      g_pending_len = 0;
      g_ptt = false;
      g_tx_finishing = false;
      stopVoiceWorker();
      strcpy(_status, "Send failed - try again");
      ui.requestDraw();
    }
  }
}

// Light kick from the UI loop — real decode is on c2dec.
// Never start c2dec while TX holds the floor.
void VoiceScreen::pollRxPlayback() {
  if (g_call_state != CALL_CONNECTED || g_ptt || g_tx_finishing) return;
  ensureRxWorker();
  if (g_c2.ready() && jitterCount() > 0)
    ensureDecWorker();
}

void VoiceScreen::onPacketSent(uint32_t tag, uint16_t len, bool eos) {
  VoiceAckSlot& s = g_voice_acks[g_voice_ack_idx];
  s.tag       = tag;
  s.sent_ms   = millis();
  s.len       = len;
  s.eos       = eos;
  s.acked     = false;
  s.timed_out = false;
  s.retx      = 0;
  g_voice_ack_idx = (g_voice_ack_idx + 1) % VOICE_ACK_SLOTS;
  Serial.printf("[voice] registered tag=%u len=%u eos=%d\n", tag, len, eos ? 1 : 0);
}

void VoiceScreen::onTargetAdvert(const ContactInfo& contact) {
  if (!g_missed_pending) return;
  if (memcmp(contact.id.pub_key, g_missed_prefix, 6) != 0) return;
  if (g_missed_sends < MISSED_CALL_MAX_SENDS) {
    if (g_missed_last_try != 0 && millis() - g_missed_last_try < 15000)
      return;
    missedCallSendOnce(ui);
    ui.requestDraw();
  }
}
#endif