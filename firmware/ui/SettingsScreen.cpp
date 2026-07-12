#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/TxtDataHelpers.h>

enum SetItem : int {
  SI_NAME = 0, SI_FREQ, SI_SF, SI_BW, SI_CR, SI_POWER, SI_PRESET,
  SI_BRIGHT, SI_TIMEOUT, SI_ALWAYS, SI_SOUND, SI_VOL, SI_FLIP, SI_TOUCHMAP, SI_TBSPEED, SI_ADVINT,
  SI_LOCPOL, SI_MANLAT, SI_MANLON, SI_AUTOADD,
  SI_ADVERT, SI_ADVERTF, SI_SDMAPS, SI_SDUPDATE, SI_ABOUT,
  SI_COUNT
};

static const char* LABELS[SI_COUNT] = {
  "Node name", "Frequency (MHz)", "Spreading factor", "Bandwidth (kHz)", "Coding rate", "TX power (dBm)", "Radio preset setup",
  "Brightness", "Screen timeout (s)", "Always-on clock", "Sounds", "Volume", "Flip display", "Touch mapping", "Trackball speed", "Auto-advert (min)",
  "Share location in advert", "Manual latitude", "Manual longitude", "Auto-add contacts",
  "Send advert (0-hop)", "Send advert (flood)", "Reload SD map packs", "Update firmware from SD", "About"
};

#define S_ROW_H 18
#define S_TOP (STATUS_H + 4)
#define S_VIS ((SCREEN_H - S_TOP - 4) / S_ROW_H)

void SettingsScreen::enter() {
  _editing = false;
}

static bool isTextItem(int i) {
  return i == SI_NAME || i == SI_FREQ || i == SI_MANLAT || i == SI_MANLON;
}

void SettingsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("Settings");
  c.setTextSize(1);

  if (_sel < _top) _top = _sel;
  if (_sel >= _top + S_VIS) _top = _sel - S_VIS + 1;

  NodePrefs* p = ui.prefs;
  for (int i = _top; i < SI_COUNT && i < _top + S_VIS; i++) {
    int y = S_TOP + (i - _top) * S_ROW_H;
    bool sel = i == _sel;
    if (sel) c.fillRoundRect(2, y - 1, SCREEN_W - 4, S_ROW_H - 1, 4, C_BG_RAISED);
    c.setTextColor(sel ? C_FG : C_FG_DIM);
    c.setCursor(8, y + 4);
    c.print(LABELS[i]);

    char v[40] = "";
    switch (i) {
      case SI_NAME:    ellipsize(v, 18, p->node_name); break;
      case SI_FREQ:    snprintf(v, sizeof(v), "%s", StrHelper::ftoa3(p->freq)); break;
      case SI_SF:      snprintf(v, sizeof(v), "%d", p->sf); break;
      case SI_BW:      snprintf(v, sizeof(v), "%s", StrHelper::ftoa(p->bw)); break;
      case SI_CR:      snprintf(v, sizeof(v), "%d", p->cr); break;
      case SI_POWER:   snprintf(v, sizeof(v), "%d", p->tx_power_dbm); break;
      case SI_BRIGHT:  snprintf(v, sizeof(v), "%d%%", ui.set.brightness * 100 / 255); break;
      case SI_TIMEOUT: if (ui.set.timeout_s) snprintf(v, sizeof(v), "%d", ui.set.timeout_s); else strcpy(v, "never"); break;
      case SI_ALWAYS:  strcpy(v, ui.set.always_on ? "on" : "off"); break;
      case SI_SOUND:   strcpy(v, ui.set.sounds ? "on" : "off"); break;
      case SI_VOL:     snprintf(v, sizeof(v), "%d/10", ui.set.volume); break;
      case SI_FLIP:    strcpy(v, ui.set.flip ? "yes" : "no"); break;
      case SI_TOUCHMAP: snprintf(v, sizeof(v), "%c", 'A' + ui.set.touch_map); break;
      case SI_TBSPEED:  snprintf(v, sizeof(v), "%d/5", ui.set.tb_speed); break;
      case SI_ADVINT:   if (ui.set.adv_interval_min) snprintf(v, sizeof(v), "%d", ui.set.adv_interval_min); else strcpy(v, "off"); break;
      case SI_PRESET:   strcpy(v, ">"); break;
      case SI_LOCPOL:  strcpy(v, p->advert_loc_policy ? "yes" : "no"); break;
      case SI_MANLAT:  snprintf(v, sizeof(v), "%.5f", ui.set.man_lat / 1000000.0); break;
      case SI_MANLON:  snprintf(v, sizeof(v), "%.5f", ui.set.man_lon / 1000000.0); break;
      case SI_AUTOADD: strcpy(v, (p->manual_add_contacts & 1) ? "manual" : "auto"); break;
      case SI_SDMAPS:  snprintf(v, sizeof(v), "%d loaded  >", ui.sdmaps.count()); break;
      case SI_ADVERT: case SI_ADVERTF: case SI_SDUPDATE: strcpy(v, ">"); break;
      case SI_ABOUT:   snprintf(v, sizeof(v), "v%s", MESHDECK_VERSION); break;
    }

    if (_editing && i == _sel) {
      _edit[_elen] = 0;
      c.fillRect(SCREEN_W - 130, y, 126, S_ROW_H - 2, C_BG);
      c.drawRect(SCREEN_W - 130, y, 126, S_ROW_H - 2, C_ACCENT);
      c.setTextColor(C_YELLOW);
      c.setCursor(SCREEN_W - 124, y + 4);
      c.print(_edit);
      c.fillRect(SCREEN_W - 124 + _elen * 6 + 1, y + 3, 2, 10, C_ACCENT);
    } else {
      c.setTextColor(sel ? C_ACCENT : C_FG_FAINT);
      c.setCursor(SCREEN_W - 8 - strlen(v) * 6, y + 4);
      c.print(v);
    }
  }

  // hint bar
  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 10);
  if (_editing) c.print("type value, enter = apply");
  else if (_sel == SI_TBSPEED) {
    // live input test: roll the ball, click it, press keys - watch this update
    bool btn, touch; int px, py; uint8_t key;
    ui.hw.inputDebug(btn, px, py, key, touch);
    c.setTextColor(btn ? C_GREEN : C_FG_FAINT);
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "click:%s ball:%d,%d key:%d(%c) kb:%s touch:%s",
             btn ? "DOWN" : "up", px, py, key, (key >= 32 && key < 127) ? key : '.',
             ui.hw.hasKeyboard() ? "Y" : "N", touch ? "Y" : "N");
    c.print(dbg);
  }
  else if (isTextItem(_sel)) c.print("enter = edit value");
  else c.print("left/right = change   enter = action");
}

void SettingsScreen::adjust(int dir) {
  NodePrefs* p = ui.prefs;
  bool radio = false;
  switch (_sel) {
    case SI_SF:    p->sf = constrain(p->sf + dir, 7, 12); radio = true; break;
    case SI_CR:    p->cr = constrain(p->cr + dir, 5, 8); radio = true; break;
    case SI_POWER: p->tx_power_dbm = constrain(p->tx_power_dbm + dir, 1, 22); radio = true; break;
    case SI_BW: {
      static const float BWS[] = { 62.5f, 125.0f, 250.0f, 500.0f };
      int bi = 0;
      for (int i = 0; i < 4; i++) if (fabsf(p->bw - BWS[i]) < 0.1f) bi = i;
      bi = constrain(bi + dir, 0, 3);
      p->bw = BWS[bi];
      radio = true;
      break;
    }
    case SI_BRIGHT: {
      int b = ui.set.brightness + dir * 25;
      ui.set.brightness = constrain(b, 30, 255);
      ui.hw.setBacklight(ui.set.brightness);
      break;
    }
    case SI_TIMEOUT: {
      static const uint16_t TOS[] = { 0, 15, 30, 60, 120, 300 };
      int ti = 0;
      for (int i = 0; i < 6; i++) if (ui.set.timeout_s == TOS[i]) ti = i;
      ti = constrain(ti + dir, 0, 5);
      ui.set.timeout_s = TOS[ti];
      break;
    }
    case SI_ALWAYS: ui.set.always_on = !ui.set.always_on; break;
    case SI_SOUND:  ui.set.sounds = !ui.set.sounds; ui.hw.setSound(ui.set.sounds, ui.set.volume); break;
    case SI_VOL:
      ui.set.volume = constrain(ui.set.volume + dir, 0, 10);
      ui.hw.setSound(ui.set.sounds, ui.set.volume);
      if (dir > 0) ui.hw.beep(1319, 50);
      break;
    case SI_FLIP:
      ui.set.flip = !ui.set.flip;
      ui.hw.setRotationFlip(ui.set.flip);
      break;
    case SI_TOUCHMAP:
      ui.set.touch_map = (ui.set.touch_map + (dir > 0 ? 1 : 3)) & 3;
      ui.hw.setTouchMap(ui.set.touch_map);
      ui.toast("Tap the screen to test");
      break;
    case SI_TBSPEED:
      ui.set.tb_speed = constrain(ui.set.tb_speed + dir, 1, 5);
      ui.hw.setTrackballStep(6 - ui.set.tb_speed);
      break;
    case SI_ADVINT: {
      static const uint8_t IVS[] = { 0, 5, 15, 30, 60 };
      int ii = 0;
      for (int i = 0; i < 5; i++) if (ui.set.adv_interval_min == IVS[i]) ii = i;
      ii = constrain(ii + dir, 0, 4);
      ui.set.adv_interval_min = IVS[ii];
      break;
    }
    case SI_LOCPOL: p->advert_loc_policy = p->advert_loc_policy ? 0 : 1; break;
    case SI_AUTOADD: p->manual_add_contacts ^= 1; break;
    default: return;
  }
  if (radio) ui.mesh->applyRadioPrefs();
  ui.mesh->savePrefs();
  ui.saveSettings();
}

void SettingsScreen::select() {
  NodePrefs* p = ui.prefs;
  switch (_sel) {
    case SI_NAME:
      _editing = true;
      StrHelper::strncpy(_edit, p->node_name, sizeof(_edit));
      _elen = strlen(_edit);
      break;
    case SI_FREQ:
      _editing = true;
      snprintf(_edit, sizeof(_edit), "%s", StrHelper::ftoa3(p->freq));
      _elen = strlen(_edit);
      break;
    case SI_MANLAT:
      _editing = true;
      snprintf(_edit, sizeof(_edit), "%.5f", ui.set.man_lat / 1000000.0);
      _elen = strlen(_edit);
      break;
    case SI_MANLON:
      _editing = true;
      snprintf(_edit, sizeof(_edit), "%.5f", ui.set.man_lon / 1000000.0);
      _elen = strlen(_edit);
      break;
    case SI_PRESET:
      ui.go(SCR_ONBOARD);
      break;
    case SI_ADVERT:
      ui.mesh->advert();
      ui.toast("Zero-hop advert sent");
      break;
    case SI_ADVERTF:
      ui.mesh->advertFlood();
      ui.toast("Flood advert sent");
      break;
    case SI_SDMAPS:
      ui.reloadSDMaps();
      break;
    case SI_SDUPDATE: {
      ui.toast("Reading SD card...", C_YELLOW);
      const char* err = ui.hw.updateFromSD();   // only returns on failure
      ui.toast(err, C_RED);
      break;
    }
    case SI_ABOUT:
      ui.termLog(C_TERM_SYS, "MeshDeck v%s | MeshCore %s | built " FIRMWARE_BUILD_DATE, MESHDECK_VERSION, FIRMWARE_VERSION);
      ui.toast("MeshDeck v" MESHDECK_VERSION " - see terminal");
      break;
    default:
      adjust(1);
      break;
  }
}

void SettingsScreen::applyEdit() {
  _edit[_elen] = 0;
  NodePrefs* p = ui.prefs;
  switch (_sel) {
    case SI_NAME:
      if (_elen > 0) {
        StrHelper::strncpy(p->node_name, _edit, sizeof(p->node_name));
        ui.mesh->savePrefs();
        ui.toast("Name saved - advert to announce");
      }
      break;
    case SI_FREQ: {
      float f = atof(_edit);
      if (f > 100 && f < 1000) {
        p->freq = f;
        ui.mesh->applyRadioPrefs();
        ui.mesh->savePrefs();
        ui.toast("Frequency updated");
      } else ui.toast("Invalid frequency", C_RED);
      break;
    }
    case SI_MANLAT: {
      ui.set.man_lat = (int32_t)(atof(_edit) * 1000000.0);
      ui.saveSettings();
      ui.applySettings();
      if (ui.sensors) { ui.sensors->node_lat = ui.set.man_lat / 1000000.0; }
      break;
    }
    case SI_MANLON: {
      ui.set.man_lon = (int32_t)(atof(_edit) * 1000000.0);
      ui.saveSettings();
      ui.applySettings();
      if (ui.sensors) { ui.sensors->node_lon = ui.set.man_lon / 1000000.0; }
      break;
    }
  }
  _editing = false;
}

bool SettingsScreen::key(uint8_t k) {
  if (_editing) {
    if (k == 0x0D) { applyEdit(); return true; }
    if (k == 0x08) {
      if (_elen > 0) _elen--;
      else _editing = false;
      return true;
    }
    if (k >= 32 && k < 127 && _elen < (int)sizeof(_edit) - 2) {
      _edit[_elen++] = k;
      return true;
    }
    return true;
  }
  if (k == 0x0D) { select(); return true; }
  return false;
}

bool SettingsScreen::nav(NavEvent e) {
  if (_editing) {
    if (e == NAV_SELECT) { applyEdit(); return true; }
    if (e == NAV_BACK) { _editing = false; return true; }
    return true;
  }
  switch (e) {
    case NAV_UP:    if (_sel > 0) _sel--; return true;
    case NAV_DOWN:  if (_sel < SI_COUNT - 1) _sel++; return true;
    case NAV_LEFT:  adjust(-1); return true;
    case NAV_RIGHT: adjust(1); return true;
    case NAV_SELECT: select(); return true;
    default: return false;
  }
}
