#include "AllScreens.h"
#include "../MyMesh.h"
#include <helpers/sensors/LocationProvider.h>
#include <math.h>

/*
 * GPS Info: live fix status from the location provider (not just "a position
 * is set"), plus quick actions to centre the map or set the manual fallback
 * position visually on the map.
 *
 * HDOP and 2D/3D fix type are parsed inside MeshCore's NMEA provider but not
 * exposed through its LocationProvider interface, so they show "n/a".
 */

#define GI_ROW   13
#define GI_LBL_X 10
#define GI_VAL_X 104
#define GI_ACT_H 22
#define GI_ACT_Y (SCREEN_H - 14 - 3 * GI_ACT_H)

static void fmtAge(char* out, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60)        snprintf(out, n, "%lus ago", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum ago", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh ago", (unsigned long)(s / 3600));
}

int GpsInfoScreen::nActions() const {
  return ui.hasManualPos() ? 3 : 2;   // Center, Set manual, [Clear manual]
}

static const char* actionLabel(int i) {
  switch (i) {
    case 0:  return "Center map on me";
    case 1:  return "Set manual position on map";
    default: return "Clear manual position";
  }
}

void GpsInfoScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_BG);
  ui.drawStatusBar("GPS Info");
  c.setTextSize(1);

  UITask::GpsState st = ui.gpsState();
  LocationProvider* loc = ui.sensors ? ui.sensors->getLocationProvider() : nullptr;
  bool active = st == UITask::GPS_SEARCHING || st == UITask::GPS_FIX || st == UITask::GPS_LOST;
  uint32_t now = millis();
  char v[48];
  int y = STATUS_H + 8;

  auto row = [&](const char* label, const char* val, uint16_t col) {
    c.setTextColor(C_FG_DIM);
    c.setCursor(GI_LBL_X, y);
    c.print(label);
    c.setTextColor(col);
    c.setCursor(GI_VAL_X, y);
    c.print(val);
    y += GI_ROW;
  };

  // status
  switch (st) {
    case UITask::GPS_FIX:       row("Status", "Fix", C_GREEN); break;
    case UITask::GPS_SEARCHING: row("Status", "Searching...", C_YELLOW); break;
    case UITask::GPS_LOST: {
      char a[16]; fmtAge(a, sizeof(a), now - ui.gpsLastFixMs());
      snprintf(v, sizeof(v), "Fix lost (%s)", a);
      row("Status", v, C_YELLOW);
      break;
    }
    case UITask::GPS_NO_MODULE: row("Status", "No module detected", C_RED); break;
    default:                    row("Status", "Off", C_FG_FAINT); break;
  }

  // satellites (reported while searching too: useful to see it's working)
  if (active && loc) snprintf(v, sizeof(v), "%ld", loc->satellitesCount());
  else strcpy(v, "-");
  row("Satellites", v, C_FG);

  // position actually in use, and where it came from
  double lat = 0, lon = 0;
  const char* src = "none";
  uint16_t src_col = C_FG_FAINT;
  if (st == UITask::GPS_FIX && loc) {
    lat = loc->getLatitude() / 1000000.0;
    lon = loc->getLongitude() / 1000000.0;
    src = "GPS (live)"; src_col = C_GREEN;
  } else if (ui.ownPos(lat, lon)) {
    bool manual = ui.hasManualPos() &&
                  fabs(lat - ui.set.man_lat / 1000000.0) < 0.0000015 &&
                  fabs(lon - ui.set.man_lon / 1000000.0) < 0.0000015;
    // otherwise it's the last fix this boot, or the position MeshCore saved
    src = manual ? "Manual" : ui.gpsLastFixMs() ? "Last GPS fix" : "Saved position";
    src_col = C_YELLOW;
  }
  if (lat != 0 || lon != 0) {
    snprintf(v, sizeof(v), "%.5f %c", fabs(lat), lat >= 0 ? 'N' : 'S');
    row("Latitude", v, C_FG);
    snprintf(v, sizeof(v), "%.5f %c", fabs(lon), lon >= 0 ? 'E' : 'W');
    row("Longitude", v, C_FG);
  } else {
    row("Latitude", "-", C_FG_FAINT);
    row("Longitude", "-", C_FG_FAINT);
  }
  row("Source", src, src_col);

  if (st == UITask::GPS_FIX && loc) snprintf(v, sizeof(v), "%.0f m", loc->getAltitude() / 1000.0);
  else strcpy(v, "-");
  row("Altitude", v, C_FG);

  if (st == UITask::GPS_FIX) strcpy(v, "live");
  else if (ui.gpsLastFixMs()) fmtAge(v, sizeof(v), now - ui.gpsLastFixMs());
  else strcpy(v, "never");
  row("Last fix", v, C_FG);

  row("HDOP / type", "n/a", C_FG_FAINT);

  if (ui.hasManualPos()) {
    char ll[40];
    MapOverlay::fmtLatLon(ll, sizeof(ll), ui.set.man_lat / 1000000.0, ui.set.man_lon / 1000000.0);
    row("Manual", ll, C_FG_DIM);
  } else {
    row("Manual", "not set", C_FG_FAINT);
  }

  // one-line hint for the states that need user action
  const char* hint = nullptr;
  if (st == UITask::GPS_OFF)            hint = "Turn on: Settings > GPS module";
  else if (st == UITask::GPS_NO_MODULE) hint = "Connect a GPS module, then reboot";
  else if (st == UITask::GPS_SEARCHING) hint = "Needs open sky; first fix can take minutes";
  if (hint) {
    c.setTextColor(C_FG_FAINT);
    c.setCursor(GI_LBL_X, y + 2);
    c.print(hint);
  }

  // actions
  for (int i = 0; i < nActions(); i++) {
    int ay = GI_ACT_Y + i * GI_ACT_H;
    bool on = i == _sel;
    c.fillRoundRect(8, ay, SCREEN_W - 16, GI_ACT_H - 3, 4, on ? C_BG_RAISED : C_BG_ALT);
    if (on) c.drawRoundRect(8, ay, SCREEN_W - 16, GI_ACT_H - 3, 4, C_ACCENT);
    c.setTextColor(on ? C_FG : C_FG_DIM);
    c.setCursor(18, ay + 6);
    c.print(actionLabel(i));
  }
  c.setTextColor(C_FG_FAINT);
  c.setCursor(8, SCREEN_H - 11);
  c.print("up/down  enter=ok  back=close");
}

void GpsInfoScreen::runAction(int i) {
  if (i == 0) {
    double lat, lon;
    if (!ui.ownPos(lat, lon)) { ui.toast("No position yet", C_YELLOW); return; }
    ui.openMap(UITask::MAPREQ_CENTER);
  } else if (i == 1) {
    ui.openMap(UITask::MAPREQ_PICK);
  } else if (i == 2 && ui.hasManualPos()) {
    ui.clearManualPos();
    if (_sel >= nActions()) _sel = nActions() - 1;
    ui.toast("Manual position cleared", C_FG_DIM);
  }
}

bool GpsInfoScreen::key(uint8_t k) {
  if (k == 0x0D) { runAction(_sel); return true; }
  if (k >= '1' && k < '1' + nActions()) { _sel = k - '1'; runAction(_sel); return true; }
  return false;                              // backspace bubbles up = back
}

bool GpsInfoScreen::nav(NavEvent e) {
  if (e == NAV_UP)     { if (_sel > 0) _sel--; return true; }
  if (e == NAV_DOWN)   { if (_sel < nActions() - 1) _sel++; return true; }
  if (e == NAV_SELECT) { runAction(_sel); return true; }
  return false;                              // BACK bubbles up
}

bool GpsInfoScreen::touch(const TouchEvent& e) {
  if (e.kind != TouchEvent::TAP) return false;
  if (e.y < GI_ACT_Y) return false;
  int i = (e.y - GI_ACT_Y) / GI_ACT_H;
  if (i >= 0 && i < nActions()) { _sel = i; runAction(i); return true; }
  return false;
}
