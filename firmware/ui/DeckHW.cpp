#include "DeckHW.h"
#include "Theme.h"
#include <esp_heap_caps.h>
#include <driver/i2s.h>
#include <SD.h>
#include <Update.h>

volatile int16_t DeckHW::_tb_x = 0;
volatile int16_t DeckHW::_tb_y = 0;
volatile bool DeckHW::_click_edge = false;

void IRAM_ATTR DeckHW::isrUp()    { _tb_y--; }
void IRAM_ATTR DeckHW::isrDown()  { _tb_y++; }
void IRAM_ATTR DeckHW::isrLeft()  { _tb_x--; }
void IRAM_ATTR DeckHW::isrRight() { _tb_x++; }
void IRAM_ATTR DeckHW::isrClick() { _click_edge = true; }

// PSRAM-backed canvas: GFXcanvas16 allocates with malloc which lands in PSRAM
// for large blocks on this board (SPIRAM_USE_MALLOC), so we can use it directly.

bool DeckHW::begin(bool flip_display) {
  _flip = flip_display;

  // backlight PWM (LEDC channel 1; T-Deck backlight is on GPIO42)
  ledcSetup(1, 20000, 8);
  ledcAttachPin(TDECK_TFT_BL, 1);
  ledcWrite(1, 255);

  // display on its own HSPI handle, same wiring pattern as stock MeshCore T-Deck build
  // (MISO is attached once here so the SD card can share the bus without re-routing
  //  pins later - re-routing would steal SCK/MOSI from the radio's SPI host)
  _spi = new SPIClass(HSPI);
  _spi->begin(TDECK_SPI_SCK, TDECK_SPI_MISO, TDECK_SPI_MOSI, TDECK_TFT_CS);
  _tft = new Adafruit_ST7789(_spi, TDECK_TFT_CS, TDECK_TFT_DC, -1);
  _tft->init(240, 320);           // panel is portrait native
  _tft->setRotation(_flip ? 1 : 3);
  _tft->setSPISpeed(40000000);
  _tft->fillScreen(0x0000);

  _canvas = new GFXcanvas16(SCREEN_W, SCREEN_H);

  // NOTE: we deliberately do NOT touch the I2C bus here. MeshCore's radio_init()
  // calls Wire.begin() itself; calling Wire.begin() twice (here AND there) leaves
  // the ESP32 I2C driver wedged, which kills the keyboard and touch. All I2C
  // setup and device probing is done once, in initI2CDevices(), AFTER radio_init.

  // trackball
  pinMode(TDECK_TB_UP, INPUT_PULLUP);
  pinMode(TDECK_TB_DOWN, INPUT_PULLUP);
  pinMode(TDECK_TB_LEFT, INPUT_PULLUP);
  pinMode(TDECK_TB_RIGHT, INPUT_PULLUP);
  pinMode(TDECK_TB_PRESS, INPUT_PULLUP);
  // FALLING edge: one clean pulse per roll-detent (CHANGE double-counts and
  // cancels out on a mechanical trackball)
  attachInterrupt(TDECK_TB_UP, isrUp, FALLING);
  attachInterrupt(TDECK_TB_DOWN, isrDown, FALLING);
  attachInterrupt(TDECK_TB_LEFT, isrLeft, FALLING);
  attachInterrupt(TDECK_TB_RIGHT, isrRight, FALLING);
  _click_edge = false;
  attachInterrupt(TDECK_TB_PRESS, isrClick, FALLING);   // catch every click in hardware

  _last_activity = millis();
  return true;
}

// Called ONCE from main, just before the sensor I2C scan. Recovers a wedged
// bus, (re)starts Wire cleanly, sets a sane clock/timeout, and probes the
// keyboard + touch. THE BUS-RECOVERY IS THE KEY FIX: a device left holding SDA
// low (e.g. interrupted mid-read on the previous boot) stalls every I2C
// transaction, which killed the keyboard AND touch AND hung boot for ~2 min.
void DeckHW::initI2CDevices() {
  // Release the I2C driver so we can drive the pins by hand.
  Wire.end();
  pinMode(TDECK_I2C_SCL, OUTPUT_OPEN_DRAIN);
  pinMode(TDECK_I2C_SDA, INPUT_PULLUP);
  digitalWrite(TDECK_I2C_SCL, HIGH);
  delayMicroseconds(10);
  // Clock SCL until the slave releases SDA (max 16 pulses).
  for (int i = 0; i < 16 && digitalRead(TDECK_I2C_SDA) == LOW; i++) {
    digitalWrite(TDECK_I2C_SCL, LOW);  delayMicroseconds(10);
    digitalWrite(TDECK_I2C_SCL, HIGH); delayMicroseconds(10);
  }
  // Generate a STOP condition (SDA low->high while SCL high) to reset slaves.
  pinMode(TDECK_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(TDECK_I2C_SDA, LOW);  delayMicroseconds(10);
  digitalWrite(TDECK_I2C_SCL, HIGH); delayMicroseconds(10);
  digitalWrite(TDECK_I2C_SDA, HIGH); delayMicroseconds(10);

  // Clean (re)start of the I2C bus.
  Wire.begin(TDECK_I2C_SDA, TDECK_I2C_SCL);
  Wire.setClock(100000);      // 100 kHz - reliable for these peripherals
  Wire.setTimeOut(20);        // 20 ms, not the 1000 ms default: an absent device
                              // must never stall the UI loop for a second

  pinMode(TDECK_TOUCH_INT, INPUT);   // GT911 free-runs with INT as input

  // Probe the keyboard (ESP32-C3 at 0x55). It boots its own firmware after
  // peripheral power comes up, so retry for up to ~2 s.
  for (int attempt = 0; attempt < 20 && !_kb_present; attempt++) {
    Wire.beginTransmission((uint8_t)TDECK_KB_ADDR);
    if (Wire.endTransmission() == 0) { _kb_present = true; break; }
    delay(100);
  }

  // Probe GT911 touch at both addresses. If not found, _touch_addr stays 0 and
  // we never poll it (polling an absent device wastes bus time).
  const uint8_t addrs[2] = { 0x5D, 0x14 };
  for (int attempt = 0; attempt < 4 && _touch_addr == 0; attempt++) {
    for (int i = 0; i < 2; i++) {
      Wire.beginTransmission(addrs[i]);
      if (Wire.endTransmission() == 0) { _touch_addr = addrs[i]; break; }
    }
    if (_touch_addr == 0) delay(30);
  }
}

void DeckHW::setRotationFlip(bool flip) {
  _flip = flip;
  if (_tft) _tft->setRotation(flip ? 1 : 3);
}

void DeckHW::push() {
  if (_tft && _canvas && _disp_on) {
    _tft->drawRGBBitmap(0, 0, _canvas->getBuffer(), SCREEN_W, SCREEN_H);
  }
}

void DeckHW::setBacklight(uint8_t level) {
  _bl_level = level;
  if (_disp_on) ledcWrite(1, level);
}

void DeckHW::displayOff() {
  if (_disp_on) {
    ledcWrite(1, 0);
    _tft->enableSleep(true);
    _disp_on = false;
  }
}

void DeckHW::displayOn() {
  if (!_disp_on) {
    _tft->enableSleep(false);
    delay(5);
    _disp_on = true;
    push();
    ledcWrite(1, _bl_level);
  }
}

// ---------------- keyboard ----------------

uint8_t DeckHW::readKey() {
  // throttle to ~60 Hz - the keyboard's C3 returns each key once and can drop
  // presses if polled thousands of times a second
  uint32_t now = millis();
  if (now - _last_kb_read < 16) return 0;
  _last_kb_read = now;

  if (Wire.requestFrom((uint8_t)TDECK_KB_ADDR, (uint8_t)1) != 1) return 0;
  if (Wire.available()) {
    uint8_t k = Wire.read();
    if (k != 0 && k != 0xFF) {      // 0x00 / 0xFF are idle values
      _kb_present = true;           // a real key proves the keyboard is alive
      _last_key = k;
      _last_activity = millis();
      return k;
    }
  }
  return 0;
}

// ---------------- trackball + button ----------------

NavEvent DeckHW::readNav() {
  uint32_t now = millis();

  // Button (GPIO0, active low). Debounced. Fire SELECT the moment a press is
  // confirmed (feels instant); a hold past 550 ms cancels the pending select
  // and fires BACK on release instead.
  // Trackball click model - read the SAME way the (reliable) directions are:
  // the press edge is caught by a hardware interrupt (isrClick sets _click_edge),
  // so a quick tap can never be missed even if this loop samples slowly. We then
  // poll the pin only to tell a tap from a hold:
  //   * quick tap  -> SELECT (on release)
  //   * hold >450ms -> BACK
  // A button already held at boot produces no falling edge, so it can't fire.
  bool low = digitalRead(TDECK_TB_PRESS) == LOW;

  if (_click_edge && !_btn_was_down) {            // fresh press detected in hardware
    _click_edge = false;
    _btn_was_down = true;
    _btn_down_at = now;
    _btn_long_fired = false;
    _btn_up_since = 0;
    _last_activity = now;
    noInterrupts(); _tb_x = 0; _tb_y = 0; interrupts();   // a click never scrolls
  }
  if (_btn_was_down) {
    if (low) {
      _btn_up_since = 0;
      if (!_btn_long_fired && now - _btn_down_at > 450) {
        _btn_long_fired = true;
        _last_activity = now;
        return NAV_BACK;                          // held -> back
      }
    } else {                                       // pin is high (released) - debounce it
      if (_btn_up_since == 0) _btn_up_since = now;
      if (now - _btn_up_since > 25) {
        _btn_was_down = false;
        _click_edge = false;                       // drop any bounce edge from the release
        _last_activity = now;
        if (!_btn_long_fired) return NAV_SELECT;   // quick tap -> select
      }
    }
  }

  // Trackball: a physical roll fires a burst of pulses. Emit one nav step once
  // _tb_step pulses accumulate on an axis, rate-limited so a fast roll doesn't
  // fly through the whole list but stays responsive.
  const int TH = _tb_step;
  if (now - _last_nav_ms < 25) return NAV_NONE;

  noInterrupts();
  int16_t x = _tb_x, y = _tb_y;
  interrupts();

  NavEvent ev = NAV_NONE;
  if (y <= -TH)      { noInterrupts(); _tb_y = 0; _tb_x = 0; interrupts(); ev = _flip ? NAV_DOWN : NAV_UP; }
  else if (y >=  TH) { noInterrupts(); _tb_y = 0; _tb_x = 0; interrupts(); ev = _flip ? NAV_UP : NAV_DOWN; }
  else if (x <= -TH) { noInterrupts(); _tb_x = 0; _tb_y = 0; interrupts(); ev = _flip ? NAV_RIGHT : NAV_LEFT; }
  else if (x >=  TH) { noInterrupts(); _tb_x = 0; _tb_y = 0; interrupts(); ev = _flip ? NAV_LEFT : NAV_RIGHT; }

  if (ev != NAV_NONE) { _last_nav_ms = now; _last_activity = now; }
  return ev;
}

// ---------------- GT911 touch ----------------

bool DeckHW::gt911Read(uint8_t* buf) {
  // status register 0x814E, point data 0x814F..
  Wire.beginTransmission(_touch_addr);
  Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(_touch_addr, (uint8_t)9);
  if (Wire.available() < 9) return false;
  for (int i = 0; i < 9; i++) buf[i] = Wire.read();
  if (buf[0] & 0x80) {
    // clear status
    Wire.beginTransmission(_touch_addr);
    Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
    Wire.endTransmission();
    return true;
  }
  return false;
}

bool DeckHW::readTouch(TouchEvent& ev) {
  ev.kind = TouchEvent::NONE;
  if (_touch_addr == 0) return false;

  uint8_t buf[9];
  bool fresh = gt911Read(buf);
  uint8_t n = fresh ? (buf[0] & 0x0F) : 0;

  if (n > 0) {
    int16_t rx = buf[2] | (buf[3] << 8);
    int16_t ry = buf[4] | (buf[5] << 8);
    // Touch controller configs vary between T-Deck batches; four mappings,
    // selectable in Settings -> Touch mapping:
    //   0 = landscape direct, 1 = landscape 180, 2 = portrait swap A, 3 = portrait swap B
    int16_t sx, sy;
    switch (_touch_map) {
      default:
      case 0: sx = rx;       sy = ry;       break;
      case 1: sx = 320 - rx; sy = 240 - ry; break;
      case 2: sx = ry;       sy = 240 - rx; break;
      case 3: sx = 320 - ry; sy = rx;       break;
    }
    if (_flip) { sx = 320 - sx; sy = 240 - sy; }
    if (sx < 0) sx = 0; if (sx >= SCREEN_W) sx = SCREEN_W - 1;
    if (sy < 0) sy = 0; if (sy >= SCREEN_H) sy = SCREEN_H - 1;

    _last_activity = millis();
    if (!_touching) {
      _touching = true;
      _t_start_x = _tx = sx; _t_start_y = _ty = sy;
      _t_start_ms = millis();
      _t_moved = false;
      // Raw + mapped coords for touch calibration (visible in the USB serial log).
      // raw = straight off the GT911; map<n>-> = after the selected transform.
      Serial.printf("touch raw=%d,%d  map%d-> %d,%d\n", rx, ry, _touch_map, sx, sy);
      return false;
    }
    int16_t dx = sx - _tx, dy = sy - _ty;
    if (abs(sx - _t_start_x) > 8 || abs(sy - _t_start_y) > 8) _t_moved = true;
    _tx = sx; _ty = sy;
    if (_t_moved && (dx || dy)) {
      ev.kind = TouchEvent::DRAG;
      ev.x = sx; ev.y = sy; ev.dx = dx; ev.dy = dy;
      return true;
    }
    return false;
  }

  if (_touching && fresh) {   // finger lifted
    _touching = false;
    if (!_t_moved && millis() - _t_start_ms < 600) {
      ev.kind = TouchEvent::TAP;
      ev.x = _tx; ev.y = _ty; ev.dx = 0; ev.dy = 0;
    } else {
      ev.kind = TouchEvent::RELEASE;
      ev.x = _tx; ev.y = _ty;
      ev.dx = _tx - _t_start_x; ev.dy = _ty - _t_start_y;
    }
    return true;
  }
  return false;
}

// ---------------- sound (I2S speaker) ----------------

void DeckHW::i2sTone(uint16_t freq, uint16_t ms) {
  if (!_snd_on || _snd_vol == 0 || freq == 0) return;

  const int RATE = 16000;
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = TDECK_I2S_BCK;
  pins.ws_io_num = TDECK_I2S_WS;
  pins.data_out_num = TDECK_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins);

  int16_t amp = 1500 * _snd_vol;      // vol 0..10
  int total = RATE * ms / 1000;
  int half = RATE / freq / 2;
  if (half < 1) half = 1;
  int16_t sample[2];
  int level = amp;
  int cnt = 0;
  size_t written;
  for (int i = 0; i < total; i++) {
    if (++cnt >= half) { cnt = 0; level = -level; }
    sample[0] = sample[1] = level;
    if (i2s_write(I2S_NUM_0, sample, sizeof(sample), &written, pdMS_TO_TICKS(50)) != ESP_OK) break;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
}

void DeckHW::beep(uint16_t f, uint16_t ms) { i2sTone(f, ms); }
void DeckHW::chimeMessage() { i2sTone(1319, 60); i2sTone(1760, 90); }
void DeckHW::chimeBoot()    { i2sTone(880, 70); i2sTone(1109, 70); i2sTone(1319, 110); }
void DeckHW::chimeError()   { i2sTone(220, 120); }

// ---------------- SD card ----------------
// The bus already has MISO attached (see begin()), so no pin re-routing is
// needed - that would break the radio, which shares these pins on another host.

bool DeckHW::sdBegin() {
  return SD.begin(TDECK_SD_CS, *_spi, 10000000);
}

void DeckHW::sdEnd() {
  SD.end();
}

// ---------------- SD firmware update ----------------

const char* DeckHW::updateFromSD() {
  if (!sdBegin()) {
    sdEnd();
    return "No SD card found";
  }
  File f = SD.open("/firmware.bin");
  if (!f) {
    sdEnd();
    return "firmware.bin not on card";
  }
  size_t sz = f.size();
  if (!Update.begin(sz)) { f.close(); SD.end(); return "Not enough space"; }

  uint8_t buf[4096];
  size_t done = 0;
  while (done < sz) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    Update.write(buf, n);
    done += n;
  }
  f.close();
  SD.end();
  if (done == sz && Update.end(true)) {
    ESP.restart();   // never returns
  }
  return "Update failed - try again";
}
