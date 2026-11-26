#include "Battery.h"

// Battery sense pin (1:1 divider on the T-Deck) and divider correction.
// 2.0 is the nominal divider. 2.04 is measured on a real T-Deck (2026-11-26):
// with Meshtastic's 2.11 a freshly charged, unplugged cell read 4.30 V, which a
// LiPo can't rest at (max ~4.2 V), so 2.11 over-reads ~3% here. Override with
// -D MESHDECK_BATT_MULT=... if a multimeter on the cell says otherwise.
#ifndef MESHDECK_BATT_PIN
#define MESHDECK_BATT_PIN 4
#endif
#ifndef MESHDECK_BATT_MULT
#define MESHDECK_BATT_MULT 2.04f
#endif

namespace {

const uint32_t SAMPLE_MS      = 1000;  // one averaged sample per second
const int      ADC_SAMPLES    = 16;    // averaged per sample
const float    EMA_ALPHA      = 1.0f / 16.0f;  // ~16 s time constant, for the percentage

// USB-power inference (no hardware charge pin on the T-Deck).
// Measured on a real T-Deck: with USB attached the sense pin reads ~4.6 V (it
// sees USB power, not the cell); unplugged, a full cell reads ~4.15 V. So
// plug/unplug is a ~450 mV step either way - far bigger than anything a
// discharging cell does (load changes / relaxation are tens of mV).
// Transitions use the median of the last 3 one-second samples: responds in
// ~2 s and ignores a single TX-sag or glitch sample.
//  - plug-in:  median rises PLUG_RISE_MV above the lowest level seen on battery
//  - unplug:   median falls UNPLUG_DROP_MV below the highest level seen on USB
//  - ABS_ON_MV: no LiPo reads this high, so it means USB even with no step to
//    see (booting with the charger already attached). Must sit well above a
//    full cell, or an unplugged full cell flips straight back to "charging".
const float    PLUG_RISE_MV   = 250.0f;
const float    UNPLUG_DROP_MV = 200.0f;
const float    ABS_ON_MV      = 4400.0f;

const int      MED_N      = 3;
bool     g_init     = false;
uint32_t g_last_ms  = 0;
uint16_t g_raw_mv   = 0;
float    g_med_buf[MED_N];
int      g_med_i    = 0;
float    g_ema      = 0;   // slow filter, drives the percentage
float    g_trough   = 0;   // lowest median since last unplug
float    g_peak     = 0;   // highest median since last plug-in
bool     g_charging = false;
int      g_pct      = -1;

void medFill(float v) { for (int i = 0; i < MED_N; i++) g_med_buf[i] = v; }
float medPush(float v) {
  g_med_buf[g_med_i] = v;
  g_med_i = (g_med_i + 1) % MED_N;
  float a = g_med_buf[0], b = g_med_buf[1], c = g_med_buf[2];
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

// LiPo state of charge vs voltage under light load (same table Meshtastic uses
// for this class of cell). Descending, 10% steps.
const uint16_t OCV[] = { 4190, 4050, 3990, 3890, 3800, 3720, 3630, 3530, 3420, 3300, 3100 };
const int OCV_N = sizeof(OCV) / sizeof(OCV[0]);

int pctFromMv(float mv) {
  if (mv <= 0) return -1;
  if (mv >= OCV[0]) return 100;
  if (mv <= OCV[OCV_N - 1]) return 0;
  for (int i = 1; i < OCV_N; i++) {
    if (mv >= OCV[i]) {
      float hi = OCV[i - 1], lo = OCV[i];
      float pct_hi = 100.0f - (i - 1) * 10.0f;
      return (int)(pct_hi - 10.0f * (hi - mv) / (hi - lo) + 0.5f);
    }
  }
  return 0;
}

uint16_t readOnce() {
  uint32_t acc = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) acc += analogReadMilliVolts(MESHDECK_BATT_PIN);
  return (uint16_t)((float)acc / ADC_SAMPLES * MESHDECK_BATT_MULT + 0.5f);
}

void updatePercent() {
  if (g_charging) return;                    // can't see the cell through the charger
  int p = pctFromMv(g_ema);
  // 1-point hysteresis so the number doesn't flicker between neighbours
  if (g_pct < 0 || p > g_pct + 1 || p < g_pct - 1 || p == 0 || p == 100) g_pct = p;
}

void sample() {
  g_last_ms = millis();
  g_raw_mv = readOnce();
  float m = g_raw_mv;

  if (!g_init) {
    g_init = true;
    medFill(m);
    g_ema = g_trough = g_peak = m;
    g_charging = m >= ABS_ON_MV;
    if (!g_charging) updatePercent();
    return;
  }

  float med = medPush(m);
  g_ema += (m - g_ema) * EMA_ALPHA;

  if (!g_charging) {
    if (med < g_trough) g_trough = med;
    if (med >= g_trough + PLUG_RISE_MV || med >= ABS_ON_MV) {
      // Hold the level from before the plug-in (the slow filter has barely
      // moved yet, and the trough is the pre-plug level).
      g_charging = true;
      if (g_pct >= 0) g_pct = pctFromMv(g_trough);
      g_peak = med;
      return;
    }
    updatePercent();
  } else {
    if (med > g_peak) g_peak = med;
    if (med <= g_peak - UNPLUG_DROP_MV) {
      // Unplugged: restart every filter from the current (cell) reading so the
      // percentage appears right away instead of sliding down from USB level.
      g_charging = false;
      medFill(m);
      g_ema = g_trough = m;
      g_pct = -1;
      updatePercent();
    }
  }
}

} // namespace

namespace Battery {

void update() {
  if (!g_init || millis() - g_last_ms >= SAMPLE_MS) sample();
}

uint16_t milliVolts() {
  if (!g_init) sample();
  return (uint16_t)(g_ema + 0.5f);
}

uint16_t rawMilliVolts() {
  if (!g_init) sample();
  return g_raw_mv;
}

int percent() {
  if (!g_init) sample();
  return g_pct;
}

bool charging() {
  if (!g_init) sample();
  return g_charging;
}

} // namespace Battery
