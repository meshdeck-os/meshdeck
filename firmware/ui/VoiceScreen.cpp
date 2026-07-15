#include "AllScreens.h"
#include <Wire.h>

/*
 * Voice test (BETA only) - proof of concept for audio capture + playback on the
 * T-Deck's ES7210 mic array and I2S speaker. Everything real is gated behind
 * MESHDECK_BETA; in the stable build this screen just says "beta only".
 *
 * This is a first pass meant to be tuned from the serial log: it prints the
 * ES7210 chip-id read-back (confirms the I2C link), how many samples were
 * captured, and the peak/RMS level (confirms the mic is actually hearing you).
 */

#define VOICE_HZ    16000
#define VOICE_SECS  2
#define VOICE_CAP   (VOICE_HZ * VOICE_SECS)   // samples

#ifdef MESHDECK_BETA
#include <driver/i2s.h>
#include <esp_heap_caps.h>

// ---- ES7210 mic (I2C control) ----
// Per LilyGo's T-Deck source the ES7210 lives on the SAME Wire bus as the
// keyboard/touch (SDA=18, SCL=8) at 7-bit address 0x40 (strap options 0x40..0x43).
// The chip has NO chip-id register - presence is a plain address ACK, and reg
// 0x40..0x4C are the chip's own analog/gain registers (they just happen to
// overlap the number 0x40). The old code read 0xFD/0xFE (that's the ES8311's
// id) so it could never pass.
static uint8_t g_es_addr = 0x40;   // resolved by the address probe below
// ES7210 mic I2S bus (separate from the speaker bus)
#define ES_MCLK        48
#define ES_SCK         47
#define ES_LRCK        21
#define ES_DIN         14
// speaker I2S bus (same pins DeckHW uses for beeps)
#define SPK_BCK        7
#define SPK_WS         5
#define SPK_DOUT       6
#define BOARD_POWERON  10

static void es_w(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(g_es_addr);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t es_r(uint8_t reg) {
  Wire.beginTransmission(g_es_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;    // repeated start
  if (Wire.requestFrom((uint8_t)g_es_addr, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

// Ground truth: log every device that ACKs on the shared I2C bus. We should see
// the keyboard (0x55) and touch (0x14 or 0x5D); the ES7210 should show 0x40..0x43.
static void i2c_scan() {
  Serial.println("[voice] I2C scan (shared bus SDA18/SCL8):");
  int n = 0;
  for (uint8_t a = 1; a < 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      const char* who = (a == 0x55) ? " (keyboard)"
                      : (a == 0x14 || a == 0x5D) ? " (touch GT911)"
                      : (a >= 0x40 && a <= 0x43) ? " (ES7210 mic?)" : "";
      Serial.printf("[voice]   0x%02X ACK%s\n", a, who);
      n++;
    }
  }
  Serial.printf("[voice] scan done: %d device(s)\n", n);
}

// Power the rail, scan the bus, and resolve the ES7210 address. No register
// writes here - the codec is a SLAVE and must be configured while its MCLK is
// running, which only happens once the I2S driver is started (see record()).
static bool es7210_probe() {
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);   // gates the whole peripheral rail
  delay(50);

  i2c_scan();

  bool found = false;
  for (uint8_t a = 0x40; a <= 0x43; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { g_es_addr = a; found = true; break; }
  }
  Serial.printf("[voice] ES7210 probe: %s (using 0x%02X)\n",
                found ? "FOUND" : "no ACK on 0x40-0x43", g_es_addr);
  return found;
}

// Configure the ES7210 for 16 kHz / 16-bit / I2S slave, MIC1+MIC2.
// MUST be called AFTER the I2S master (MCLK) is running - the ADC clock domain
// needs MCLK present during bring-up, otherwise SDOUT stays flat 0 forever.
static bool es7210_config() {
  es_w(0x00, 0xFF); es_w(0x00, 0x41);   // reset, release
  es_w(0x01, 0x1F);                      // clock manager (fixed up to 0x14 below)
  es_w(0x09, 0x30); es_w(0x0A, 0x30);    // time control 0/1
  es_w(0x40, 0xC3);                      // analog: VMID / vdda
  es_w(0x41, 0x70); es_w(0x42, 0x70);    // mic bias 1/2, 3/4
  es_w(0x07, 0x20);                      // OSR
  es_w(0x02, 0xC1);                      // main clock: adc_div=1, doubler, dll (16k, 256fs)
  es_w(0x04, 0x01); es_w(0x05, 0x00);    // LRCK divider (16 kHz)
  es_w(0x11, 0x60); es_w(0x12, 0x00);    // SDP out: I2S, 16-bit
  es_w(0x14, 0x00); es_w(0x15, 0x00);    // ADC12/ADC34 UN-MUTE (take SDOUT out of mute)
  // ---- mic_select(MIC1|MIC2): the analog power-up ----
  es_w(0x43, 0x00); es_w(0x44, 0x00); es_w(0x45, 0x00); es_w(0x46, 0x00);
  es_w(0x4B, 0xFF); es_w(0x4C, 0xFF);    // transient power-down
  es_w(0x01, 0x14);                      // enable ADC1/2 clocks (clear 0x0B mask)
  es_w(0x4B, 0x00);                      // *** MIC1/2 bias + ADC + PGA POWER ON ***
  es_w(0x43, 0x10); es_w(0x44, 0x10);    // MIC1/MIC2 enable bit4
  // ---- gain: ~30 dB for a clear first test (low nibble; 0x0A=30dB) ----
  es_w(0x43, 0x1A); es_w(0x44, 0x1A);
  // ---- start(): final power-up needed for capture ----
  es_w(0x06, 0x00);                      // power-down register: fully up
  es_w(0x47, 0x00); es_w(0x48, 0x00);    // MIC1/MIC2 individual power on
  es_w(0x49, 0x00); es_w(0x4A, 0x00);
  es_w(0x4B, 0x00); es_w(0x4C, 0xFF);    // re-assert MIC1/2 on, 3/4 off

  uint8_t r0 = es_r(0x00), r4b = es_r(0x4B), r06 = es_r(0x06);
  Serial.printf("[voice] ES7210 configured @ 0x%02X, reg00=0x%02X reg4B=0x%02X(pwr) reg06=0x%02X\n",
                g_es_addr, r0, r4b, r06);
  return true;
}

static void mic_i2s_start() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = VOICE_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ALL_LEFT;   // LilyGo: ES7210 mic sits in the LEFT slot
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 64;                             // match LilyGo
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_pin_config_t pins = {};
  pins.mck_io_num = ES_MCLK;
  pins.bck_io_num = ES_SCK;
  pins.ws_io_num = ES_LRCK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = ES_DIN;
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
}
static void mic_i2s_stop() { i2s_driver_uninstall(I2S_NUM_1); }

static void spk_i2s_start() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = VOICE_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = SPK_BCK;
  pins.ws_io_num = SPK_WS;
  pins.data_out_num = SPK_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins);
}
static void spk_i2s_stop() { i2s_zero_dma_buffer(I2S_NUM_0); i2s_driver_uninstall(I2S_NUM_0); }
#endif  // MESHDECK_BETA

void VoiceScreen::enter() {
#ifdef MESHDECK_BETA
  if (!_buf) {
    _buf = (int16_t*)heap_caps_malloc(VOICE_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    _cap = _buf ? VOICE_CAP : 0;
  }
  _hwok = es7210_probe();
  strcpy(_status, _hwok ? "mic ready - ENTER to record" : "mic not on I2C - see scan log");
#else
  strcpy(_status, "beta build only");
#endif
}

void VoiceScreen::record() {
#ifdef MESHDECK_BETA
  if (!_buf) { strcpy(_status, "no audio buffer"); return; }
  strcpy(_status, "recording 2s...");
  ui.requestDraw();
  // Start the I2S master FIRST so MCLK is live, THEN configure the ES7210 while
  // it is clocked. Discard the first ~100 ms (clock/PLL settle, DC transient).
  mic_i2s_start();
  delay(30);
  es7210_config();
  delay(30);
  size_t junk = 0;
  for (int i = 0; i < 8; i++) {           // flush settle samples
    int16_t tmp[128];
    i2s_read(I2S_NUM_1, (char*)tmp, sizeof(tmp), &junk, pdMS_TO_TICKS(50));
  }
  _len = 0;
  size_t got = 0;
  uint32_t t0 = millis();
  while (_len < _cap && millis() - t0 < 4000) {
    size_t br = 0;
    int want = (_cap - _len) * sizeof(int16_t);
    if (want > 2048) want = 2048;
    if (i2s_read(I2S_NUM_1, (char*)(_buf + _len), want, &br, pdMS_TO_TICKS(200)) != ESP_OK) break;
    _len += br / sizeof(int16_t);
    got += br;
  }
  mic_i2s_stop();
  // level stats
  int peak = 0; long long acc = 0;
  for (int i = 0; i < _len; i++) {
    int s = _buf[i]; if (s < 0) s = -s;
    if (s > peak) peak = s;
    acc += (long long)_buf[i] * _buf[i];
  }
  _peak = peak;
  _rms = _len ? (int)sqrt((double)(acc / _len)) : 0;
  Serial.printf("[voice] captured %d samples (%u bytes), peak=%d rms=%d\n", _len, (unsigned)got, _peak, _rms);
  snprintf(_status, sizeof(_status), "got %d smp  peak %d", _len, _peak);
#endif
}

void VoiceScreen::playback() {
#ifdef MESHDECK_BETA
  if (_len <= 0) { strcpy(_status, "nothing recorded yet"); return; }
  strcpy(_status, "playing...");
  ui.requestDraw();
  spk_i2s_start();
  int16_t stereo[128];               // duplicate mono -> L/R for the speaker amp
  int i = 0;
  while (i < _len) {
    int n = 0;
    while (n < 64 && i < _len) { stereo[n * 2] = _buf[i]; stereo[n * 2 + 1] = _buf[i]; n++; i++; }
    size_t wr = 0;
    i2s_write(I2S_NUM_0, (const char*)stereo, n * 2 * sizeof(int16_t), &wr, pdMS_TO_TICKS(200));
  }
  spk_i2s_stop();
  Serial.printf("[voice] played %d samples\n", _len);
  strcpy(_status, "played - ENTER to record again");
#endif
}

void VoiceScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Voice test (beta)");
  c.setTextSize(1);

#ifndef MESHDECK_BETA
  c.setTextColor(C_YELLOW);
  c.setCursor(12, 70);
  c.print("Experimental - flash the beta");
  c.setCursor(12, 84);
  c.print("build to try voice capture.");
  return;
#else
  int y = STATUS_H + 14;
  c.setTextColor(_hwok ? C_GREEN : C_RED);
  c.setCursor(12, y);
  c.print(_hwok ? "ES7210 mic: detected" : "ES7210 mic: NOT found");
  y += 18;

  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Status");
  c.setTextColor(C_FG);     c.setCursor(90, y); c.print(_status);  y += 16;
  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Samples");
  { char v[16]; snprintf(v, sizeof(v), "%d", _len); c.setTextColor(C_FG); c.setCursor(90, y); c.print(v); } y += 16;
  c.setTextColor(C_FG_DIM); c.setCursor(12, y); c.print("Peak / RMS");
  { char v[20]; snprintf(v, sizeof(v), "%d / %d", _peak, _rms); c.setTextColor(C_FG); c.setCursor(90, y); c.print(v); } y += 18;

  // level bar (peak, 0..32767 -> 0..width)
  int bw = SCREEN_W - 24;
  c.drawRect(12, y, bw, 12, C_FG_FAINT);
  int fill = _peak > 0 ? (int)((long)_peak * (bw - 2) / 32767) : 0;
  c.fillRect(13, y + 1, fill, 10, _peak > 24000 ? C_RED : C_GREEN);
  y += 26;

  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 22);
  c.print("ENTER / R = record 2s");
  c.setCursor(6, SCREEN_H - 10);
  c.print("P = play back   (watch the serial log)");
#endif
}

bool VoiceScreen::key(uint8_t k) {
#ifdef MESHDECK_BETA
  if (k == 0x0D || k == 'r' || k == 'R') { record(); return true; }
  if (k == 'p' || k == 'P') { playback(); return true; }
#endif
  return false;
}

bool VoiceScreen::nav(NavEvent e) {
#ifdef MESHDECK_BETA
  if (e == NAV_SELECT) { record(); return true; }
#endif
  return false;   // BACK falls through to UITask (leaves the screen)
}
