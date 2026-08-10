#include "AllScreens.h"
#include "../MyMesh.h"
#include "NewMap.h"
#include <math.h>
#include <string.h>

// Zoom steps (px per degree lon) — denser than classic Map for street detail
static const float NM_ZOOMS[] = {
  8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
};
#define NM_N_ZOOMS ((int)(sizeof(NM_ZOOMS) / sizeof(NM_ZOOMS[0])))

// Professional basemap palette (RGB565)
#define NM_BG           RGB565(232, 236, 228)   // soft paper land
#define NM_WATER        RGB565(170, 210, 235)
#define NM_WATER_LINE   RGB565(100, 160, 200)
#define NM_PARK         RGB565(190, 220, 170)
#define NM_FOREST       RGB565(150, 190, 140)
#define NM_RESIDENTIAL  RGB565(235, 230, 220)
#define NM_INDUSTRIAL   RGB565(220, 210, 200)
#define NM_BUILDING     RGB565(200, 195, 185)
#define NM_BUILDING_OL  RGB565(170, 165, 155)
#define NM_RAIL         RGB565(110, 100, 95)
#define NM_PATH         RGB565(180, 160, 130)
#define NM_SVC          RGB565(200, 200, 195)
#define NM_RES_ROAD     RGB565(255, 255, 255)
#define NM_RES_CASE     RGB565(180, 180, 175)
#define NM_TERT         RGB565(255, 250, 220)
#define NM_TERT_CASE    RGB565(200, 180, 100)
#define NM_SEC          RGB565(255, 235, 150)
#define NM_SEC_CASE     RGB565(210, 170, 60)
#define NM_PRI          RGB565(255, 210, 120)
#define NM_PRI_CASE     RGB565(200, 140, 40)
#define NM_TRUNK        RGB565(255, 170, 100)
#define NM_TRUNK_CASE   RGB565(200, 100, 40)
#define NM_MWY          RGB565(240, 140, 100)
#define NM_MWY_CASE     RGB565(180, 70, 40)
#define NM_LABEL        RGB565(50, 55, 60)
#define NM_LABEL_HALO   RGB565(245, 245, 240)
#define NM_GRID         RGB565(210, 215, 205)

static int nmZoomIndex(float scale) {
  int best = 0;
  float bd = 1e9f;
  for (int i = 0; i < NM_N_ZOOMS; i++) {
    float d = fabsf(scale - NM_ZOOMS[i]);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

// Thick line with optional casing (draw dark wide, then bright narrow)
static void nmDrawSeg(GFXcanvas16& c, int x0, int y0, int x1, int y1,
                      uint16_t fill, uint16_t cas, int half_w) {
  if (half_w <= 0) {
    c.drawLine(x0, y0, x1, y1, fill);
    return;
  }
  // Integer perpendicular offsets for smooth-looking multi-stroke roads
  int dx = x1 - x0, dy = y1 - y0;
  int steps = half_w;
  // Approximate unit normal via integer
  float len = sqrtf((float)(dx * dx + dy * dy));
  if (len < 0.5f) {
    c.fillCircle(x0, y0, half_w, fill);
    return;
  }
  float nx = -(float)dy / len;
  float ny = (float)dx / len;

  // Casing first (slightly wider, darker)
  if (cas != fill) {
    for (int s = -half_w - 1; s <= half_w + 1; s++) {
      int ox = (int)lroundf(nx * s);
      int oy = (int)lroundf(ny * s);
      c.drawLine(x0 + ox, y0 + oy, x1 + ox, y1 + oy, cas);
    }
  }
  for (int s = -half_w; s <= half_w; s++) {
    int ox = (int)lroundf(nx * s);
    int oy = (int)lroundf(ny * s);
    c.drawLine(x0 + ox, y0 + oy, x1 + ox, y1 + oy, fill);
  }
  // Round caps
  c.fillCircle(x0, y0, half_w, fill);
  c.fillCircle(x1, y1, half_w, fill);
}

// Soft polyline: densify long segments so curves look continuous on screen
static void nmDrawPolyLine(GFXcanvas16& c, const int* xs, const int* ys, int n,
                           uint16_t fill, uint16_t cas, int half_w) {
  if (n < 2) return;
  for (int i = 1; i < n; i++) {
    int x0 = xs[i - 1], y0 = ys[i - 1], x1 = xs[i], y1 = ys[i];
    int adx = abs(x1 - x0), ady = abs(y1 - y0);
    int segs = (adx > ady ? adx : ady) / 12;  // subdivide long stretches
    if (segs < 1) segs = 1;
    if (segs > 8) segs = 8;
    int px = x0, py = y0;
    for (int s = 1; s <= segs; s++) {
      int x = x0 + (x1 - x0) * s / segs;
      int y = y0 + (y1 - y0) * s / segs;
      nmDrawSeg(c, px, py, x, y, fill, cas, half_w);
      px = x; py = y;
    }
  }
}

// Simple scanline-ish fill for small closed polys (axis-aligned span fill)
static void nmFillPoly(GFXcanvas16& c, int* xs, int* ys, int n, uint16_t col) {
  if (n < 3) return;
  // Clip n for stack
  if (n > 64) n = 64;
  int miny = ys[0], maxy = ys[0];
  for (int i = 1; i < n; i++) {
    if (ys[i] < miny) miny = ys[i];
    if (ys[i] > maxy) maxy = ys[i];
  }
  if (maxy < 0 || miny >= SCREEN_H) return;
  if (miny < 0) miny = 0;
  if (maxy >= SCREEN_H) maxy = SCREEN_H - 1;
  // Cap work for large areas
  if (maxy - miny > 120) {
    // Outline only for huge polys
    for (int i = 0; i < n; i++) {
      int j = (i + 1) % n;
      c.drawLine(xs[i], ys[i], xs[j], ys[j], col);
    }
    return;
  }
  for (int y = miny; y <= maxy; y++) {
    int nx = 0;
    int xint[32];
    for (int i = 0; i < n && nx < 32; i++) {
      int j = (i + 1) % n;
      int y0 = ys[i], y1 = ys[j], x0 = xs[i], x1 = xs[j];
      if (y0 == y1) continue;
      if ((y < y0 && y < y1) || (y >= y0 && y >= y1)) continue;
      float t = (float)(y - y0) / (float)(y1 - y0);
      xint[nx++] = x0 + (int)lroundf(t * (x1 - x0));
    }
    // sort
    for (int a = 0; a + 1 < nx; a++)
      for (int b = a + 1; b < nx; b++)
        if (xint[b] < xint[a]) { int t = xint[a]; xint[a] = xint[b]; xint[b] = t; }
    for (int k = 0; k + 1 < nx; k += 2) {
      int xa = xint[k], xb = xint[k + 1];
      if (xb < 0 || xa >= SCREEN_W) continue;
      if (xa < 0) xa = 0;
      if (xb >= SCREEN_W) xb = SCREEN_W - 1;
      c.drawFastHLine(xa, y, xb - xa + 1, col);
    }
  }
}

void NewMapsScreen::enter() {
  if (!_centered_once) {
    double lat, lon;
    if (ui.ownPos(lat, lon)) {
      _clat = lat; _clon = lon;
      _scale = 256;
    } else {
      // Northern Idaho default (Coeur d'Alene area) when no GPS
      const NewMapPack* p = ui.newmaps.pack(0);
      if (p && p->loaded) {
        _clat = 0.5 * (p->lat_min + p->lat_max);
        _clon = 0.5 * (p->lon_min + p->lon_max);
        _scale = 192;
      } else {
        _clat = 47.68; _clon = -116.78; _scale = 256;
      }
    }
    _centered_once = true;
  }
}

void NewMapsScreen::project(double lat, double lon, int& x, int& y) const {
  double ys = _scale / cos(_clat * 0.017453292519943295);
  x = (int)lround((lon - _clon) * _scale) + SCREEN_W / 2;
  y = SCREEN_H / 2 - (int)lround((lat - _clat) * ys);
}

static bool nmOnScreen(int x, int y) {
  return x > -80 && x < SCREEN_W + 80 && y > -80 && y < SCREEN_H + 80;
}

struct LayerStyle {
  uint16_t fill, cas;
  int half_w;
  bool fill_poly;
};

static LayerStyle nmStyle(uint8_t layer, float scale) {
  LayerStyle s{ NM_RES_ROAD, NM_RES_CASE, 0, false };
  auto roadW = [&](int base) {
    if (scale >= 1024) return base + 1;
    if (scale >= 384) return base;
    if (scale >= 128) return base > 0 ? base - 0 : 0;
    return 0;
  };
  switch (layer) {
    case NML_WATER_AREA:
      s = { NM_WATER, NM_WATER, 0, true }; break;
    case NML_LANDUSE_PARK:
      s = { NM_PARK, NM_PARK, 0, true }; break;
    case NML_LANDUSE_FOREST:
      s = { NM_FOREST, NM_FOREST, 0, true }; break;
    case NML_LANDUSE_RESIDENTIAL:
      s = { NM_RESIDENTIAL, NM_RESIDENTIAL, 0, true }; break;
    case NML_LANDUSE_INDUSTRIAL:
      s = { NM_INDUSTRIAL, NM_INDUSTRIAL, 0, true }; break;
    case NML_WATER_LINE:
      s = { NM_WATER_LINE, NM_WATER_LINE, scale >= 256 ? 1 : 0, false }; break;
    case NML_RAIL:
      s = { NM_RAIL, NM_RAIL, 0, false }; break;
    case NML_ROAD_PATH:
      s = { NM_PATH, NM_PATH, 0, false }; break;
    case NML_ROAD_SERVICE:
      s = { NM_SVC, NM_RES_CASE, 0, false }; break;
    case NML_ROAD_RESIDENTIAL:
      s = { NM_RES_ROAD, NM_RES_CASE, roadW(1), false }; break;
    case NML_ROAD_TERTIARY:
      s = { NM_TERT, NM_TERT_CASE, roadW(1), false }; break;
    case NML_ROAD_SECONDARY:
      s = { NM_SEC, NM_SEC_CASE, roadW(1), false }; break;
    case NML_ROAD_PRIMARY:
      s = { NM_PRI, NM_PRI_CASE, roadW(2), false }; break;
    case NML_ROAD_TRUNK:
      s = { NM_TRUNK, NM_TRUNK_CASE, roadW(2), false }; break;
    case NML_ROAD_MOTORWAY:
      s = { NM_MWY, NM_MWY_CASE, roadW(2), false }; break;
    case NML_BUILDING:
      s = { NM_BUILDING, NM_BUILDING_OL, 0, true }; break;
    default: break;
  }
  return s;
}

// Draw order: areas first, then rails/paths, then roads low→high, buildings last under labels
static const uint8_t NM_DRAW_ORDER[] = {
  NML_LANDUSE_RESIDENTIAL, NML_LANDUSE_INDUSTRIAL,
  NML_LANDUSE_FOREST, NML_LANDUSE_PARK,
  NML_WATER_AREA, NML_WATER_LINE,
  NML_RAIL, NML_ROAD_PATH, NML_ROAD_SERVICE,
  NML_ROAD_RESIDENTIAL, NML_ROAD_TERTIARY, NML_ROAD_SECONDARY,
  NML_ROAD_PRIMARY, NML_ROAD_TRUNK, NML_ROAD_MOTORWAY,
  NML_BUILDING,
};

void NewMapsScreen::drawPack(const NewMapPack* pk) {
  if (!pk || !pk->loaded || !pk->pts || !pk->feats) return;
  GFXcanvas16& c = ui.cv();

  double ys = _scale / cos(_clat * 0.017453292519943295);
  double half_lon = (SCREEN_W / 2.0) / _scale;
  double half_lat = (SCREEN_H / 2.0) / ys;
  double lat0 = _clat - half_lat, lat1 = _clat + half_lat;
  double lon0 = _clon - half_lon, lon1 = _clon + half_lon;
  // pad for thick strokes
  double pad = 0.02 * (half_lon + half_lat);
  lat0 -= pad; lat1 += pad; lon0 -= pad; lon1 += pad;

  const float sc = pk->scale;
  const int16_t view_scale = (int16_t)(_scale > 65535 ? 65535 : _scale);

  int xs[96], ys_[96];

  for (unsigned oi = 0; oi < sizeof(NM_DRAW_ORDER); oi++) {
    uint8_t want = NM_DRAW_ORDER[oi];
    LayerStyle st = nmStyle(want, _scale);

    for (uint32_t fi = 0; fi < pk->n_feats; fi++) {
      const NewMapFeat& F = pk->feats[fi];
      if (F.layer != want) continue;
      if (F.min_scale > (uint16_t)view_scale) continue;
      if (F.count < 2 || F.start + F.count > pk->n_points) continue;

      // Quick bbox reject using first/mid/last samples
      auto sample = [&](uint32_t idx, double& la, double& lo) {
        const NewMapPt& p = pk->pts[idx];
        la = p.lat_s / sc;
        lo = p.lon_s / sc;
      };
      double la0, lo0, la1, lo1, la2, lo2;
      sample(F.start, la0, lo0);
      sample(F.start + F.count / 2, la1, lo1);
      sample(F.start + F.count - 1, la2, lo2);
      double fmin_lat = la0, fmax_lat = la0, fmin_lon = lo0, fmax_lon = lo0;
      auto acc = [&](double la, double lo) {
        if (la < fmin_lat) fmin_lat = la;
        if (la > fmax_lat) fmax_lat = la;
        if (lo < fmin_lon) fmin_lon = lo;
        if (lo > fmax_lon) fmax_lon = lo;
      };
      acc(la1, lo1); acc(la2, lo2);
      if (fmax_lat < lat0 || fmin_lat > lat1 || fmax_lon < lon0 || fmin_lon > lon1)
        continue;

      // Project vertices (subsample at low zoom for speed)
      int stride = 1;
      if (_scale < 64 && F.count > 40) stride = 4;
      else if (_scale < 128 && F.count > 30) stride = 3;
      else if (_scale < 256 && F.count > 24) stride = 2;

      int n = 0;
      bool any_on = false;
      for (uint32_t i = 0; i < F.count && n < 95; i += (uint32_t)stride) {
        const NewMapPt& p = pk->pts[F.start + i];
        double la = p.lat_s / sc, lo = p.lon_s / sc;
        int x, y;
        project(la, lo, x, y);
        xs[n] = x; ys_[n] = y;
        if (nmOnScreen(x, y)) any_on = true;
        n++;
      }
      // Always include last point
      if (stride > 1 && F.count >= 2) {
        const NewMapPt& p = pk->pts[F.start + F.count - 1];
        double la = p.lat_s / sc, lo = p.lon_s / sc;
        int x, y;
        project(la, lo, x, y);
        if (n == 0 || xs[n - 1] != x || ys_[n - 1] != y) {
          if (n < 95) { xs[n] = x; ys_[n] = y; n++; }
          else { xs[n - 1] = x; ys_[n - 1] = y; }
        }
        if (nmOnScreen(x, y)) any_on = true;
      }
      if (n < 2 || !any_on) continue;

      bool closed = (F.flags & NMF_CLOSED) != 0;
      if (st.fill_poly && closed && n >= 3) {
        nmFillPoly(c, xs, ys_, n, st.fill);
        // soft outline
        for (int i = 0; i < n; i++) {
          int j = (i + 1) % n;
          c.drawLine(xs[i], ys_[i], xs[j], ys_[j], st.cas != st.fill ? st.cas : st.fill);
        }
      } else {
        nmDrawPolyLine(c, xs, ys_, n, st.fill, st.cas, st.half_w);
        if (closed && n >= 3)
          nmDrawSeg(c, xs[n - 1], ys_[n - 1], xs[0], ys_[0], st.fill, st.cas, st.half_w);
      }
    }
  }

  // Labels
  if (pk->labels && _scale >= 48) {
    c.setTextSize(1);
    for (uint32_t i = 0; i < pk->n_labels; i++) {
      const NewMapLabel& L = pk->labels[i];
      uint16_t need = (uint16_t)L.min_scale_div * 4;
      if (need > (uint16_t)view_scale) continue;
      // kind filtering
      if (L.kind >= 3 && _scale < 192) continue;
      if (L.kind >= 2 && _scale < 96) continue;
      double la = L.lat_s / sc, lo = L.lon_s / sc;
      if (la < lat0 || la > lat1 || lo < lon0 || lo > lon1) continue;
      int x, y;
      project(la, lo, x, y);
      if (x < 4 || x > SCREEN_W - 40 || y < STATUS_H + 4 || y > SCREEN_H - 20) continue;
      // halo
      c.setTextColor(NM_LABEL_HALO);
      c.setCursor(x + 1, y - 2);
      c.print(L.name);
      c.setCursor(x - 1, y - 2);
      c.print(L.name);
      c.setTextColor(NM_LABEL);
      c.setCursor(x, y - 3);
      c.print(L.name);
      c.fillCircle(x - 3, y, 1, NM_LABEL);
    }
  }
}

void NewMapsScreen::drawNodes() {
  GFXcanvas16& c = ui.cv();
  bool labels = _scale >= 64;
  int n = ui.mesh ? ui.mesh->getNumContacts() : 0;
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
      c.fillTriangle(x, y - 5, x - 5, y, x + 5, y, C_MAP_RPT);
      c.fillTriangle(x - 5, y, x + 5, y, x, y + 5, C_MAP_RPT);
    } else if (ct.type == ADV_TYPE_ROOM) {
      c.fillRoundRect(x - 4, y - 4, 8, 8, 2, C_PURPLE);
    } else {
      c.fillCircle(x, y, 3, C_MAP_NODE);
      c.drawCircle(x, y, 5, C_MAP_NODE);
    }
    if (labels) {
      char nm[14];
      ellipsize(nm, sizeof(nm), ct.name);
      c.setTextColor(NM_LABEL_HALO);
      c.setCursor(x + 7, y - 2);
      c.print(nm);
      c.setTextColor(ct.type == ADV_TYPE_REPEATER ? C_MAP_RPT :
                     ct.type == ADV_TYPE_ROOM ? C_PURPLE : C_MAP_NODE);
      c.setCursor(x + 6, y - 3);
      c.print(nm);
    }
  }
  double slat, slon;
  if (ui.ownPos(slat, slon)) {
    int x, y;
    project(slat, slon, x, y);
    if (x > -20 && x < SCREEN_W + 20 && y > -20 && y < SCREEN_H + 20) {
      c.fillCircle(x, y, 5, C_MAP_SELF);
      c.drawCircle(x, y, 8, C_MAP_SELF);
      c.drawCircle(x, y, 9, NM_LABEL_HALO);
      if (labels) {
        c.setTextColor(C_MAP_SELF);
        c.setCursor(x + 10, y - 3);
        c.print("me");
      }
    }
  }
}

void NewMapsScreen::draw() {
  GFXcanvas16& c = ui.cv();
  c.fillScreen(NM_BG);

  double ys = _scale / cos(_clat * 0.017453292519943295);
  double half_lon = (SCREEN_W / 2.0) / _scale;
  double half_lat = (SCREEN_H / 2.0) / ys;
  double lat0 = _clat - half_lat, lat1 = _clat + half_lat;
  double lon0 = _clon - half_lon, lon1 = _clon + half_lon;

  // Subtle graticule
  float step = _scale < 32 ? 1.0f : _scale < 128 ? 0.25f : _scale < 512 ? 0.1f : 0.05f;
  c.setTextColor(NM_GRID);
  for (float g = floorf(lon0 / step) * step; g <= lon1; g += step) {
    int x, y; project(_clat, g, x, y);
    c.drawFastVLine(x, 0, SCREEN_H, NM_GRID);
  }
  for (float g = floorf(lat0 / step) * step; g <= lat1; g += step) {
    int x, y; project(g, _clon, x, y);
    c.drawFastHLine(0, y, SCREEN_W, NM_GRID);
  }

  const NewMapPack* pk = ui.newmaps.packFor(_clat, _clon);
  bool have = pk && pk->loaded;
  if (have) {
    // If view partially outside pack, still draw it
    drawPack(pk);
  } else {
    c.setTextColor(NM_LABEL);
    c.setTextSize(1);
    c.setCursor(36, 100);
    c.print("No NewMaps pack on SD");
    c.setCursor(24, 116);
    c.print("Put .mdv in /meshdeck-maps/");
    c.setCursor(18, 132);
    c.print("tools/gen_newmap.py --help");
  }

  drawNodes();

  // Crosshair
  c.drawFastHLine(SCREEN_W / 2 - 6, SCREEN_H / 2, 13, C_FG_DIM);
  c.drawFastVLine(SCREEN_W / 2, SCREEN_H / 2 - 6, 13, C_FG_DIM);
  c.drawCircle(SCREEN_W / 2, SCREEN_H / 2, 3, C_FG_FAINT);

  // Scale bar
  double km = 50.0 / _scale * 111.32 * cos(_clat * 0.017453292519943295);
  char sb[28];
  if (km >= 10) snprintf(sb, sizeof(sb), "%d km", (int)(km + 0.5));
  else if (km >= 1) snprintf(sb, sizeof(sb), "%.1f km", km);
  else snprintf(sb, sizeof(sb), "%d m", (int)(km * 1000 + 0.5));
  c.fillRoundRect(6, SCREEN_H - 28, 78, 18, 3, RGB565(255, 255, 255));
  c.drawRoundRect(6, SCREEN_H - 28, 78, 18, 3, NM_RES_CASE);
  c.drawFastHLine(12, SCREEN_H - 14, 50, NM_LABEL);
  c.drawFastVLine(12, SCREEN_H - 17, 6, NM_LABEL);
  c.drawFastVLine(62, SCREEN_H - 17, 6, NM_LABEL);
  c.setTextColor(NM_LABEL);
  c.setCursor(12, SCREEN_H - 26);
  c.print(sb);

  // Header
  char title[40];
  if (have)
    snprintf(title, sizeof(title), "NewMaps  z%d", nmZoomIndex(_scale));
  else
    snprintf(title, sizeof(title), "NewMaps");
  ui.drawStatusBar(title);

  // Footer hints
  c.setTextColor(C_FG_FAINT);
  c.setCursor(90, SCREEN_H - 12);
  c.print("+/- zoom  c me  ball pan");

  // Attribution (ODbL)
  c.setTextColor(C_FG_FAINT);
  c.setCursor(SCREEN_W - 92, STATUS_H + 2);
  c.print("c OSM");
}

bool NewMapsScreen::key(uint8_t k) {
  int zi = nmZoomIndex(_scale);
  if (k == '+' || k == '=' || k == 'q' || k == ']') {
    if (zi < NM_N_ZOOMS - 1) _scale = NM_ZOOMS[zi + 1];
    return true;
  }
  if (k == '-' || k == '_' || k == 'a' || k == '[') {
    if (zi > 0) _scale = NM_ZOOMS[zi - 1];
    return true;
  }
  if (k == 'c' || k == 'C') {
    double lat, lon;
    if (ui.ownPos(lat, lon)) { _clat = lat; _clon = lon; }
    else if (const NewMapPack* p = ui.newmaps.pack(0)) {
      _clat = 0.5 * (p->lat_min + p->lat_max);
      _clon = 0.5 * (p->lon_min + p->lon_max);
    }
    return true;
  }
  if (k == 'i' || k == 'I') {
    // Center Northern Idaho (default demo area)
    _clat = 47.68; _clon = -116.78; _scale = 384;
    return true;
  }
  if (k == '0') { _scale = NM_ZOOMS[0]; return true; }
  if (k == '9') { _scale = NM_ZOOMS[NM_N_ZOOMS - 1]; return true; }
  return false;
}

bool NewMapsScreen::nav(NavEvent e) {
  double ys = _scale / cos(_clat * 0.017453292519943295);
  double dlon = 28.0 / _scale, dlat = 28.0 / ys;
  switch (e) {
    case NAV_UP:    _clat += dlat; if (_clat > 85) _clat = 85; return true;
    case NAV_DOWN:  _clat -= dlat; if (_clat < -85) _clat = -85; return true;
    case NAV_LEFT:  _clon -= dlon; if (_clon < -180) _clon = -180; return true;
    case NAV_RIGHT: _clon += dlon; if (_clon > 180) _clon = 180; return true;
    case NAV_SELECT: {
      int zi = nmZoomIndex(_scale);
      _scale = NM_ZOOMS[(zi + 1) % NM_N_ZOOMS];
      return true;
    }
    default: return false;
  }
}

bool NewMapsScreen::touch(const TouchEvent& e) {
  if (e.kind == TouchEvent::DRAG) {
    double ys = _scale / cos(_clat * 0.017453292519943295);
    _clon -= e.dx / _scale;
    _clat += e.dy / ys;
    if (_clat > 85) _clat = 85;
    if (_clat < -85) _clat = -85;
    if (_clon < -180) _clon = -180;
    if (_clon > 180) _clon = 180;
    return true;
  }
  if (e.kind == TouchEvent::TAP) {
    // Tap right half zoom in, left zoom out
    int zi = nmZoomIndex(_scale);
    if (e.x > SCREEN_W / 2) {
      if (zi < NM_N_ZOOMS - 1) _scale = NM_ZOOMS[zi + 1];
    } else {
      if (zi > 0) _scale = NM_ZOOMS[zi - 1];
    }
    return true;
  }
  if (e.kind == TouchEvent::RELEASE) return true;
  return false;
}
