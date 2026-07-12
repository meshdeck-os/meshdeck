#include "AllScreens.h"
#include "../MyMesh.h"
#include "mapdata.h"
#include <helpers/AdvertDataHelpers.h>
#include <math.h>

// px per degree of longitude at each zoom step
static const float ZOOMS[] = { 1.7f, 4, 8, 16, 32, 64, 128, 256, 512 };
#define N_ZOOMS 9

void MapScreen::enter() {
  if (!_centered_once) {
    double lat, lon;
    if (ui.ownPos(lat, lon)) {
      _clat = lat; _clon = lon;
      _scale = 64;
    } else {
      // centre on first contact with a position, else UK default
      int n = ui.mesh->getNumContacts();
      for (int i = 0; i < n; i++) {
        ContactInfo ct;
        if (ui.mesh->getContactByIdx(i, ct) && (ct.gps_lat || ct.gps_lon)) {
          _clat = ct.gps_lat / 1000000.0;
          _clon = ct.gps_lon / 1000000.0;
          _scale = 32;
          break;
        }
      }
    }
    _centered_once = true;
  }
}

void MapScreen::project(double lat, double lon, int& x, int& y) const {
  double ys = _scale / cos(_clat * 0.0174533);
  x = (int)((lon - _clon) * _scale) + SCREEN_W / 2;
  y = SCREEN_H / 2 - (int)((lat - _clat) * ys);
}

static bool onscreenish(int x, int y) {
  return x > -1500 && x < SCREEN_W + 1500 && y > -1500 && y < SCREEN_H + 1500;
}

void MapScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(C_MAP_BG);

  double ys = _scale / cos(_clat * 0.0174533);
  double half_lon = (SCREEN_W / 2) / _scale;
  double half_lat = (SCREEN_H / 2) / ys;
  double lat0 = _clat - half_lat, lat1 = _clat + half_lat;
  double lon0 = _clon - half_lon, lon1 = _clon + half_lon;

  // graticule
  float step = _scale < 4 ? 30 : _scale < 16 ? 10 : _scale < 64 ? 5 : 1;
  for (float g = -180; g <= 180; g += step) {
    if (g >= lon0 && g <= lon1) {
      int x, y;
      project(0, g, x, y);
      c.drawFastVLine(x, 0, SCREEN_H, C_MAP_GRID);
    }
  }
  for (float g = -80; g <= 84; g += step) {
    if (g >= lat0 && g <= lat1) {
      int x, y;
      project(g, 0, x, y);
      c.drawFastHLine(0, y, SCREEN_W, C_MAP_GRID);
    }
  }

  // does an SD pack fully cover the current view at high zoom?
  bool sd_covers = false;
  if (_scale >= 48) {
    for (int pi = 0; pi < ui.sdmaps.count(); pi++) {
      const SDMapPack* pk = ui.sdmaps.pack(pi);
      if (pk->loaded && lon0 >= pk->lon_min && lon1 <= pk->lon_max &&
          lat0 >= pk->lat_min && lat1 <= pk->lat_max) { sd_covers = true; break; }
    }
  }

  bool in_eu = lon1 > EU_LON_MIN && lon0 < EU_LON_MAX && lat1 > EU_LAT_MIN && lat0 < EU_LAT_MAX;
  bool use_eu = in_eu && _scale >= 24 && !sd_covers;

  // coastlines
  if (!sd_covers && (!use_eu || lon0 < EU_LON_MIN || lon1 > EU_LON_MAX || lat0 < EU_LAT_MIN || lat1 > EU_LAT_MAX)) {
    for (int li = 0; li < WORLD_NLINES; li++) {
      const MapLine& L = WORLD_LINES[li];
      int px = 0, py = 0;
      bool have = false;
      for (int i = 0; i < L.count; i++) {
        const MapPt& p = WORLD_PTS[L.start + i];
        double lat = p.lat / (double)WORLD_SCALE, lon = p.lon / (double)WORLD_SCALE;
        if (lat < lat0 - 8 || lat > lat1 + 8 || lon < lon0 - 8 || lon > lon1 + 8) { have = false; continue; }
        int x, y;
        project(lat, lon, x, y);
        if (!onscreenish(x, y)) { have = false; continue; }
        if (have) c.drawLine(px, py, x, y, C_MAP_LAND);
        px = x; py = y; have = true;
      }
    }
  }
  if (use_eu) {
    for (int li = 0; li < EU_NLINES; li++) {
      const MapLine& L = EU_LINES[li];
      int px = 0, py = 0;
      bool have = false;
      for (int i = 0; i < L.count; i++) {
        const MapPt& p = EU_PTS[L.start + i];
        double lat = p.lat / (double)EU_SCALE, lon = p.lon / (double)EU_SCALE;
        if (lat < lat0 - 2 || lat > lat1 + 2 || lon < lon0 - 2 || lon > lon1 + 2) { have = false; continue; }
        int x, y;
        project(lat, lon, x, y);
        if (!onscreenish(x, y)) { have = false; continue; }
        if (have) c.drawLine(px, py, x, y, C_MAP_LAND);
        px = x; py = y; have = true;
      }
    }
  }

  // SD map packs: high-detail overlays, drawn on top when zoomed into their region
  for (int pi = 0; pi < ui.sdmaps.count(); pi++) {
    const SDMapPack* pk = ui.sdmaps.pack(pi);
    if (!pk->loaded || _scale < 48) continue;
    if (lon1 < pk->lon_min || lon0 > pk->lon_max || lat1 < pk->lat_min || lat0 > pk->lat_max) continue;
    for (uint32_t li = 0; li < pk->nlines; li++) {
      const SDMapLine& L = pk->lines[li];
      int px = 0, py = 0;
      bool have = false;
      for (uint16_t i = 0; i < L.count; i++) {
        const SDMapPt& p = pk->pts[L.start + i];
        double lat = p.lat / (double)pk->scale, lon = p.lon / (double)pk->scale;
        if (lat < lat0 - 1 || lat > lat1 + 1 || lon < lon0 - 1 || lon > lon1 + 1) { have = false; continue; }
        int x, y;
        project(lat, lon, x, y);
        if (!onscreenish(x, y)) { have = false; continue; }
        if (have) c.drawLine(px, py, x, y, C_MAP_LAND);
        px = x; py = y; have = true;
      }
    }
    // extra place labels from the pack
    if (_scale >= 64) {
      c.setTextSize(1);
      for (uint32_t i = 0; i < pk->ncities; i++) {
        const SDMapCity& ct = pk->cities[i];
        double lat = ct.lat100 / 100.0, lon = ct.lon100 / 100.0;
        if (lat < lat0 || lat > lat1 || lon < lon0 || lon > lon1) continue;
        int x, y;
        project(lat, lon, x, y);
        c.fillCircle(x, y, 1, C_MAP_CITY);
        c.setTextColor(C_MAP_CITY);
        c.setCursor(x + 4, y - 3);
        c.print(ct.name);
      }
    }
  }

  // cities: which ranks show at this zoom
  int max_rank = _scale < 3 ? 0 : _scale < 8 ? 1 : _scale < 24 ? 2 : _scale < 90 ? 3 : 5;
  bool labels = _scale >= 8;
  c.setTextSize(1);
  for (int i = 0; i < N_CITIES; i++) {
    const MapCity& ct = CITIES[i];
    if (ct.rank > max_rank) continue;
    double lat = ct.lat100 / 100.0, lon = ct.lon100 / 100.0;
    if (lat < lat0 || lat > lat1 || lon < lon0 || lon > lon1) continue;
    int x, y;
    project(lat, lon, x, y);
    c.fillCircle(x, y, 1, C_MAP_CITY);
    if (labels) {
      c.setTextColor(C_MAP_CITY);
      c.setCursor(x + 4, y - 3);
      c.print(ct.name);
    }
  }

  drawNodes();

  // crosshair
  c.drawFastHLine(SCREEN_W / 2 - 5, SCREEN_H / 2, 11, C_FG_DIM);
  c.drawFastVLine(SCREEN_W / 2, SCREEN_H / 2 - 5, 11, C_FG_DIM);

  // scale bar (approx km per 60px)
  double km = 60.0 / _scale * 111.0 * cos(_clat * 0.0174533);
  char sb[24];
  if (km >= 1) snprintf(sb, sizeof(sb), "%d km", (int)km);
  else snprintf(sb, sizeof(sb), "%d m", (int)(km * 1000));
  c.drawFastHLine(8, SCREEN_H - 10, 60, C_FG);
  c.drawFastVLine(8, SCREEN_H - 13, 6, C_FG);
  c.drawFastVLine(68, SCREEN_H - 13, 6, C_FG);
  c.setTextColor(C_FG);
  c.setCursor(74, SCREEN_H - 14);
  c.print(sb);

  // header strip
  ui.drawStatusBar("Map");
  c.setTextColor(C_FG_FAINT);
  c.setCursor(6, SCREEN_H - 26);
  c.print("+/- zoom  c centre  ball pan");
}

void MapScreen::drawNodes() {
  GFXcanvas16& c = ui.cv();
  bool labels = _scale >= 16;

  int n = ui.mesh->getNumContacts();
  c.setTextSize(1);
  for (int i = 0; i < n; i++) {
    ContactInfo ct;
    if (!ui.mesh->getContactByIdx(i, ct)) continue;
    if (ct.gps_lat == 0 && ct.gps_lon == 0) continue;
    double lat = ct.gps_lat / 1000000.0, lon = ct.gps_lon / 1000000.0;
    int x, y;
    project(lat, lon, x, y);
    if (x < -20 || x > SCREEN_W + 20 || y < -20 || y > SCREEN_H + 20) continue;
    if (ct.type == ADV_TYPE_REPEATER) {
      // orange diamond
      c.fillTriangle(x, y - 5, x - 5, y, x + 5, y, C_MAP_RPT);
      c.fillTriangle(x - 5, y, x + 5, y, x, y + 5, C_MAP_RPT);
    } else {
      c.fillCircle(x, y, 3, C_MAP_NODE);
      c.drawCircle(x, y, 5, C_MAP_NODE);
    }
    if (labels) {
      char nm[14];
      ellipsize(nm, sizeof(nm), ct.name);
      c.setTextColor(ct.type == ADV_TYPE_REPEATER ? C_MAP_RPT : C_MAP_NODE);
      c.setCursor(x + 7, y - 3);
      c.print(nm);
    }
  }

  // self
  double slat, slon;
  if (ui.ownPos(slat, slon)) {
    int x, y;
    project(slat, slon, x, y);
    if (x > -20 && x < SCREEN_W + 20 && y > -20 && y < SCREEN_H + 20) {
      c.fillCircle(x, y, 4, C_MAP_SELF);
      c.drawCircle(x, y, 7, C_MAP_SELF);
      if (labels) {
        c.setTextColor(C_MAP_SELF);
        c.setCursor(x + 9, y - 3);
        c.print("me");
      }
    }
  }
}

bool MapScreen::key(uint8_t k) {
  int zi = 0;
  for (int i = 0; i < N_ZOOMS; i++) if (fabsf(_scale - ZOOMS[i]) < 0.01f) { zi = i; break; }
  if (k == '+' || k == '=' || k == 'q') { if (zi < N_ZOOMS - 1) _scale = ZOOMS[zi + 1]; return true; }
  if (k == '-' || k == '_' || k == 'a') { if (zi > 0) _scale = ZOOMS[zi - 1]; return true; }
  if (k == 'c') {
    double lat, lon;
    if (ui.ownPos(lat, lon)) { _clat = lat; _clon = lon; }
    return true;
  }
  if (k == 'w') { _scale = ZOOMS[0]; _clat = 30; _clon = 0; return true; }   // world view
  if (k == 'u') { _scale = 16; _clat = 54.5; _clon = -3.5; return true; }    // UK view
  return false;
}

bool MapScreen::nav(NavEvent e) {
  double ys = _scale / cos(_clat * 0.0174533);
  double dlon = 30.0 / _scale, dlat = 30.0 / ys;
  switch (e) {
    case NAV_UP:    _clat += dlat; if (_clat > 84) _clat = 84; return true;
    case NAV_DOWN:  _clat -= dlat; if (_clat < -84) _clat = -84; return true;
    case NAV_LEFT:  _clon -= dlon; if (_clon < -180) _clon = -180; return true;
    case NAV_RIGHT: _clon += dlon; if (_clon > 180) _clon = 180; return true;
    case NAV_SELECT: {   // cycle zoom
      int zi = 0;
      for (int i = 0; i < N_ZOOMS; i++) if (fabsf(_scale - ZOOMS[i]) < 0.01f) { zi = i; break; }
      _scale = ZOOMS[(zi + 1) % N_ZOOMS];
      return true;
    }
    default: return false;
  }
}

bool MapScreen::touch(const TouchEvent& e) {
  if (e.kind == TouchEvent::DRAG) {
    double ys = _scale / cos(_clat * 0.0174533);
    _clon -= e.dx / _scale;
    _clat += e.dy / ys;
    if (_clat > 84) _clat = 84;
    if (_clat < -84) _clat = -84;
    return true;
  }
  if (e.kind == TouchEvent::TAP) {
    // recentre on tapped point
    double ys = _scale / cos(_clat * 0.0174533);
    _clon += (e.x - SCREEN_W / 2) / _scale;
    _clat -= (e.y - SCREEN_H / 2) / ys;
    return true;
  }
  return false;
}
