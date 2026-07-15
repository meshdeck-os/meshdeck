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
#define ES_ADDR        0x40
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
  Wire.beginTransmission(ES_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t es_r(uint8_t reg) {
  Wire.beginTransmission(ES_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)ES_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// Best-effort ES7210 init for 16 kHz / 16-bit / I2S, mic1+mic2 enabled.
// Register values are datasheet facts; the exact set is what we tune via serial.
static bool es7210_init() {
  pinMode(BOARD_POWERON, OUTPUT);
  digitalWrite(BOARD_POWERON, HIGH);
  delay(10);

  uint8_t id1 = es_r(0xFD), id2 = es_r(0xFE);   // chip id: expect 0x72, 0x10
  Serial.printf("[voice] ES7210 chip id = 0x%02X 0x%02X (expect 0x72 0x10)\n", id1, id2);

  es_w(0x00, 0xFF); delay(5);   // reset
  es_w(0x00, 0x32);             // release reset / power up
  es_w(0x01, 0x3F);             // enable clocks
  es_w(0x02, 0x00);
  es_w(0x03, 0x03);
  es_w(0x04, 0x03);
  es_w(0x06, 0x04);
  es_w(0x07, 0x20);
  es_w(0x08, 0x14);
  es_w(0x09, 0x30);
  es_w(0x0A, 0x30);
  es_w(0x0B, 0x00);
  es_w(0x11, 0x60);             // SDP format: I2S, 16-bit
  es_w(0x12, 0x00);
  es_w(0x40, 0x42);             // analog / vmid
  es_w(0x41, 0x70);             // mic bias 1/2
  es_w(0x42, 0x70);
  es_w(0x43, 0x1E);             // mic1 gain
  es_w(0x44, 0x1E);             // mic2 gain
  es_w(0x47, 0x08);
  es_w(0x48, 0x08);
  es_w(0x4B, 0x00);             // power on ADC
  es_w(0x4C, 0x00);
  es_w(0x00, 0x71);
  es_w(0x00, 0x41);             // run
  return (id1 == 0x72 && id2 == 0x10);
}

static void mic_i2s_start() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = VOICE_HZ;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
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
  _hwok = es7210_init();
  strcpy(_status, _hwok ? "mic ready - ENTER to record" : "mic id FAIL - see serial");
#else
  strcpy(_status, "beta build only");
#endif
}

void VoiceScreen::record() {
#ifdef MESHDECK_BETA
  if (!_buf) { strcpy(_status, "no audio buffer"); return; }
  strcpy(_status, "recording 2s...");
  ui.requestDraw();
  mic_i2s_start();
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
