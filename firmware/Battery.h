#pragma once
/*
 * MeshDeck battery monitor for the LilyGo T-Deck.
 *
 * Replaces MeshCore's TDeckBoard::getBattMilliVolts() for everything MeshDeck
 * shows or reports, because that one:
 *   - uses the raw, UNcalibrated ADC count (analogRead * 6.6 / 4096), which on
 *     the ESP32-S3 reads well off the real voltage (a full cell showed ~53%);
 *   - is fed into a linear 3.35-4.20 V -> 0-100% map, which is not how a LiPo
 *     discharges;
 *   - knows nothing about USB power, so the moment a charger is attached the
 *     sense node reads >= 4.2 V and the UI shows a false 100%.
 *
 * Here we read GPIO4 through the eFuse-calibrated analogReadMilliVolts(),
 * apply a divider factor measured on a real T-Deck (2.04), smooth the
 * result, map it through a LiPo discharge curve, and infer "on USB power" from
 * the voltage itself (the T-Deck has no charge-status / VBUS pin wired to the
 * ESP32, so this is the only signal available).
 *
 * All calls happen on the Arduino loop task (MyMesh + UITask), so no locking.
 */
#include <Arduino.h>

namespace Battery {
  // Sample (rate-limited to ~1/s) and update the charge-state detector.
  // Cheap to call every loop iteration.
  void update();

  // Smoothed, calibrated battery-sense voltage in mV (takes a first sample on
  // demand). While on USB power this is the charger-side voltage, not the
  // cell's resting voltage.
  uint16_t milliVolts();

  // Last instantaneous (unsmoothed) reading, for diagnostics.
  uint16_t rawMilliVolts();

  // State of charge 0..100, or -1 if unknown. While charging() is true this
  // is NOT measurable (the charger masks the cell), so it holds the last value
  // seen before USB power was detected.
  int percent();

  // True while external (USB) power is detected.
  bool charging();
}
