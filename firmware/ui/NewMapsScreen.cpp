#include "AllScreens.h"
#include "../MyMesh.h"
#include "NewMap.h"
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>

// Zoom steps (px per degree lon) - denser than classic Map for street detail
static const float NM_ZOOMS[] = {
  8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
};
#define NM_N_ZOOMS ((int)(sizeof(NM_ZOOMS) / sizeof(NM_ZOOMS[0])))

// Paper-map palette (RGB565). Hierarchy is width first, then hue.
#define NM_BG           RGB565(232, 236, 228)   // soft paper land
#define NM_WATER        RGB565(120, 178, 220)
#define NM_RIVER        RGB565(86, 148, 196)
#define NM_RIVER_CASE   RGB565(68, 124, 172)
#define NM_CANAL        RGB565(100, 164, 204)
#define NM_STREAM       RGB565(124, 178, 212)
#define NM_PARK         RGB565(190, 220, 170)
#define NM_FOREST       RGB565(150, 190, 140)
#define NM_RESIDENTIAL  RGB565(235, 230, 220)
#define NM_INDUSTRIAL   RGB565(220, 210, 200)
#define NM_BUILDING     RGB565(200, 195, 185)
#define NM_BUILDING_OL  RGB565(170, 165, 155)
#define NM_RAIL         RGB565(110, 100, 95)
#define NM_PATH         RGB565(186, 166, 138)
#define NM_DRV          RGB565(228, 226, 220)
#define NM_DRV_CASE     RGB565(186, 182, 174)
#define NM_SVC          RGB565(236, 236, 230)
#define NM_SVC_CASE     RGB565(180, 180, 174)
#define NM_RES_ROAD     RGB565(255, 255, 255)
#define NM_RES_CASE     RGB565(166, 166, 160)
#define NM_TERT         RGB565(255, 252, 228)
#define NM_TERT_CASE    RGB565(196, 176, 108)
#define NM_SEC          RGB565(255, 232, 128)
#define NM_SEC_CASE     RGB565(198, 158, 48)
#define NM_PRI          RGB565(255, 198, 96)
#define NM_PRI_CASE     RGB565(186, 126, 32)
#define NM_TRUNK        RGB565(255, 156, 84)
#define NM_TRUNK_CASE   RGB565(186, 86, 32)
#define NM_MWY          RGB565(234, 124, 88)
#define NM_MWY_CASE     RGB565(168, 60, 32)
#define NM_LABEL        RGB565(50, 55, 60)
#define NM_LABEL_HALO   RGB565(245, 245, 240)
#define NM_GRID         RGB565(210, 215, 205)

// Draw phases: areas, then ALL road casings, then ALL road fills.
// Casing-then-fill is how paper maps get graceful intersections.
enum NmPhase : uint8_t {
  NM_PH_AREA = 0,
  NM_PH_HYDRO_CASE,
  NM_PH_HYDRO_FILL,
  NM_PH_RAIL_PATH,
  NM_PH_ROAD_CASE,
  NM_PH_ROAD_FILL,
  NM_PH_BUILDING
};

static int nmZoomIndex(float scale) {
  int best = 0;
  float bd = 1e9f;
  for (int i = 0; i < NM_N_ZOOMS; i++) {
    float d = fabsf(scale - NM_ZOOMS[i]);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

// Disk stamp: overlapping circles make round joins/caps without N parallel lines.
static void nmDisk(GFXcanvas16& c, int x, int y, int r, uint16_t col) {
  if (x < -r || x >= SCREEN_W + r || y < -r || y >= SCREEN_H + r) return;
  if (r <= 0) {
    if ((unsigned)x < (unsigned)SCREEN_W && (unsigned)y < (unsigned)SCREEN_H)
      c.drawPixel(x, y, col);
    return;
  }
  if (r == 1) {
    c.drawFastHLine(x - 1, y, 3, col);
    if ((unsigned)(y - 1) < (unsigned)SCREEN_H) c.drawPixel(x, y - 1, col);
    if ((unsigned)(y + 1) < (unsigned)SCREEN_H) c.drawPixel(x, y + 1, col);
    return;
  }
  c.fillCircle(x, y, r, col);
}

static bool nmSegVis(int x0, int y0, int x1, int y1, int pad) {
  int minx = x0 < x1 ? x0 : x1, maxx = x0 > x1 ? x0 : x1;
  int miny = y0 < y1 ? y0 : y1, maxy = y0 > y1 ? y0 : y1;
  return maxx >= -pad && minx < SCREEN_W + pad &&
         maxy >= -pad && miny < SCREEN_H + pad;
}

// Capsule stroke. r=0 hairline. Overlapping disks = smooth T-junctions
// once every road's casing is drawn, then every road's fill.
static void nmCapsule(GFXcanvas16& c, int x0, int y0, int x1, int y1,
                      int r, uint16_t col) {
  if (!nmSegVis(x0, y0, x1, y1, r + 2)) return;
  if (r <= 0) {
    c.drawLine(x0, y0, x1, y1, col);
    return;
  }
  if (r == 1) {
    c.drawLine(x0, y0, x1, y1, col);
    nmDisk(c, x0, y0, 1, col);
    nmDisk(c, x1, y1, 1, col);
    return;
  }
  int dx = x1 - x0, dy = y1 - y0;
  int adx = abs(dx), ady = abs(dy);
  int steps = adx > ady ? adx : ady;
  if (steps < 1) { nmDisk(c, x0, y0, r, col); return; }
  int stride = r;  // overlap by ~one radius
  for (int s = 0; s <= steps; s += stride)
    nmDisk(c, x0 + dx * s / steps, y0 + dy * s / steps, r, col);
  nmDisk(c, x1, y1, r, col);
}

static void nmPolyCapsule(GFXcanvas16& c, const int* xs, const int* ys, int n,
                          int r, uint16_t col) {
  if (n < 2) return;
  for (int i = 1; i < n; i++)
    nmCapsule(c, xs[i - 1], ys[i - 1], xs[i], ys[i], r, col);
}

static void nmDash(GFXcanvas16& c, int x0, int y0, int x1, int y1, uint16_t col) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
  if (steps < 1) { c.drawPixel(x0, y0, col); return; }
  for (int s = 0; s <= steps; s++) {
    if ((s / 3) & 1) continue;
    int x = x0 + dx * s / steps, y = y0 + dy * s / steps;
    if ((unsigned)x < (unsigned)SCREEN_W && (unsigned)y < (unsigned)SCREEN_H)
      c.drawPixel(x, y, col);
  }
}

static void nmPolyDash(GFXcanvas16& c, const int* xs, const int* ys, int n,
                       uint16_t col) {
  if (n < 2) return;
  for (int i = 1; i < n; i++)
    nmDash(c, xs[i - 1], ys[i - 1], xs[i], ys[i], col);
}

// Scanline fill, clipped to the screen. Large lakes used to be outline-only
// (and invisible when the shore was off-screen).
static void nmFillPoly(GFXcanvas16& c, int* xs, int* ys, int n, uint16_t col) {
  if (n < 3) return;
  if (n > 128) n = 128;
  int miny = ys[0], maxy = ys[0];
  for (int i = 1; i < n; i++) {
    if (ys[i] < miny) miny = ys[i];
    if (ys[i] > maxy) maxy = ys[i];
  }
  if (maxy < 0 || miny >= SCREEN_H) return;
  if (miny < 0) miny = 0;
  if (maxy >= SCREEN_H) maxy = SCREEN_H - 1;
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

void NewMapsScreen::leave() {
  ui.newmaps.releaseAllTiles();
  _tiles_pending = false;
  _panning = false;
  _snap_ok = false;
  if (_snap) {
    heap_caps_free(_snap);
    _snap = nullptr;
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
  int half_w;       // 0 = hairline
  bool fill_poly;
  bool dashed;
};

// Visibility floor so old packs cannot force driveways/streets at region zoom.
static uint16_t nmStyleFloor(uint8_t layer, uint16_t feat_ms, uint8_t flags) {
  if (flags & NMF_SIGNED) return 8;  // I / US / state: always on
  uint16_t floor = 0;
  switch (layer) {
    case NML_ROAD_MOTORWAY: return NM_Z_MOTORWAY;
    case NML_ROAD_TRUNK:    return NM_Z_MOTORWAY;
    case NML_ROAD_PRIMARY:  return NM_Z_PRIMARY;
    case NML_ROAD_SECONDARY:return NM_Z_SECONDARY;
    case NML_ROAD_TERTIARY: return NM_Z_TERTIARY;
    case NML_ROAD_RESIDENTIAL: floor = NM_Z_RESIDENT; break;
    case NML_ROAD_SERVICE:
      floor = (flags & NMF_DRIVEWAY) ? NM_Z_DRIVEWAY : NM_Z_SERVICE;
      break;
    case NML_ROAD_PATH:     floor = NM_Z_PATH; break;
    case NML_RAIL:          floor = NM_Z_RAIL; break;
    case NML_WATER_LINE:
      if (feat_ms <= 24) floor = NM_Z_RIVER;
      else if (feat_ms <= 48) floor = NM_Z_CANAL;
      else floor = NM_Z_STREAM;
      break;
    case NML_BUILDING:      floor = NM_Z_BUILDING; break;
    case NML_LANDUSE_RESIDENTIAL:
    case NML_LANDUSE_INDUSTRIAL: floor = NM_Z_LANDUSE; break;
    case NML_LANDUSE_PARK:
    case NML_LANDUSE_FOREST: return NM_Z_PARK;
    case NML_WATER_AREA:    return NM_Z_WATER_AREA;
    default: break;
  }
  return floor > feat_ms ? floor : feat_ms;
}

// Width bands. 0 = 1px hairline. Stay thin until street-scale zoom
// (1024 ~= 23 km across, 2048 ~= 12 km).
static int nmBand(float scale) {
  if (scale >= 2048) return 4;
  if (scale >= 1024) return 3;
  if (scale >= 512) return 2;
  if (scale >= 256) return 1;
  return 0;
}

static LayerStyle nmStyle(uint8_t layer, float scale, uint16_t feat_ms, uint8_t flags) {
  LayerStyle s{ NM_RES_ROAD, NM_RES_CASE, 0, false, false };
  const int b = nmBand(scale);
  const bool drv = (flags & NMF_DRIVEWAY) != 0;

  //                    <256  256   512   1024  2048
  auto w = [&](int a, int c, int d, int e, int f) {
    int t[5] = { a, c, d, e, f };
    return t[b];
  };

  switch (layer) {
    case NML_WATER_AREA:
      s = { NM_WATER, NM_WATER, 0, true, false }; break;
    case NML_LANDUSE_PARK:
      s = { NM_PARK, NM_PARK, 0, true, false }; break;
    case NML_LANDUSE_FOREST:
      s = { NM_FOREST, NM_FOREST, 0, true, false }; break;
    case NML_LANDUSE_RESIDENTIAL:
      s = { NM_RESIDENTIAL, NM_RESIDENTIAL, 0, true, false }; break;
    case NML_LANDUSE_INDUSTRIAL:
      s = { NM_INDUSTRIAL, NM_INDUSTRIAL, 0, true, false }; break;
    case NML_WATER_LINE:
      if (feat_ms <= 24)
        s = { NM_RIVER, NM_RIVER_CASE, w(0, 1, 1, 2, 3), false, false };
      else if (feat_ms <= 48)
        s = { NM_CANAL, NM_RIVER_CASE, w(0, 0, 1, 1, 2), false, false };
      else
        s = { NM_STREAM, NM_STREAM, w(0, 0, 0, 1, 1), false, false };
      break;
    case NML_RAIL:
      s = { NM_RAIL, NM_RAIL, 0, false, true }; break;
    case NML_ROAD_PATH:
      s = { NM_PATH, NM_PATH, w(0, 0, 0, 0, 0), false, true }; break;
    case NML_ROAD_SERVICE:
      if (drv)
        s = { NM_DRV, NM_DRV_CASE, w(0, 0, 0, 0, 0), false, false };
      else
        s = { NM_SVC, NM_SVC_CASE, w(0, 0, 0, 0, 1), false, false };
      break;
    case NML_ROAD_RESIDENTIAL:
      s = { NM_RES_ROAD, NM_RES_CASE, w(0, 0, 0, 0, 1), false, false }; break;
    case NML_ROAD_TERTIARY:
      s = { NM_TERT, NM_TERT_CASE, w(0, 0, 0, 1, 1), false, false }; break;
    case NML_ROAD_SECONDARY:
      s = { NM_SEC, NM_SEC_CASE, w(0, 0, 1, 1, 2), false, false }; break;
    case NML_ROAD_PRIMARY:
      s = { NM_PRI, NM_PRI_CASE, w(0, 1, 1, 2, 2), false, false }; break;
    case NML_ROAD_TRUNK:
      s = { NM_TRUNK, NM_TRUNK_CASE, w(0, 1, 1, 2, 3), false, false }; break;
    case NML_ROAD_MOTORWAY:
      s = { NM_MWY, NM_MWY_CASE, w(1, 1, 2, 2, 3), false, false }; break;
    case NML_BUILDING:
      s = { NM_BUILDING, NM_BUILDING_OL, 0, true, false }; break;
    default: break;
  }
  return s;
}

static bool nmLayerInPhase(uint8_t layer, uint8_t phase) {
  switch (phase) {
    case NM_PH_AREA:
      return layer <= NML_LANDUSE_INDUSTRIAL || layer == NML_WATER_AREA;
    case NM_PH_HYDRO_CASE:
    case NM_PH_HYDRO_FILL:
      return layer == NML_WATER_LINE;
    case NM_PH_RAIL_PATH:
      return layer == NML_RAIL || layer == NML_ROAD_PATH;
    case NM_PH_ROAD_CASE:
    case NM_PH_ROAD_FILL:
      return layer >= NML_ROAD_SERVICE && layer <= NML_ROAD_MOTORWAY;
    case NM_PH_BUILDING:
      return layer == NML_BUILDING;
    default:
      return false;
  }
}

// Draw order: areas first, then rails/paths, then roads low->high, buildings last under labels
static const uint8_t NM_DRAW_ORDER[] = {
  NML_LANDUSE_RESIDENTIAL, NML_LANDUSE_INDUSTRIAL,
  NML_LANDUSE_FOREST, NML_LANDUSE_PARK,
  NML_WATER_AREA, NML_WATER_LINE,
  NML_RAIL, NML_ROAD_PATH, NML_ROAD_SERVICE,
  NML_ROAD_RESIDENTIAL, NML_ROAD_TERTIARY, NML_ROAD_SECONDARY,
  NML_ROAD_PRIMARY, NML_ROAD_TRUNK, NML_ROAD_MOTORWAY,
  NML_BUILDING,
};

void NewMapsScreen::drawFeatList(const NewMapPack* pk, const NewMapPt* pts, uint32_t n_pts,
                                const NewMapFeat* feats, uint32_t n_feats, uint8_t phase) {
  if (!pk || !pts || !feats || n_feats == 0) return;
  GFXcanvas16& c = ui.cv();

  double ys = _scale / cos(_clat * 0.017453292519943295);
  double half_lon = (SCREEN_W / 2.0) / _scale;
  double half_lat = (SCREEN_H / 2.0) / ys;
  double lat0 = _clat - half_lat, lat1 = _clat + half_lat;
  double lon0 = _clon - half_lon, lon1 = _clon + half_lon;
  double pad = 0.02 * (half_lon + half_lat);
  lat0 -= pad; lat1 += pad; lon0 -= pad; lon1 += pad;

  const float sc = pk->scale;
  const int16_t view_scale = (int16_t)(_scale > 65535 ? 65535 : _scale);

  const int MAXV = 128;
  int xs[128], ys_[128];

  for (unsigned oi = 0; oi < sizeof(NM_DRAW_ORDER); oi++) {
    uint8_t want = NM_DRAW_ORDER[oi];
    if (!nmLayerInPhase(want, phase)) continue;

    for (uint32_t fi = 0; fi < n_feats; fi++) {
      const NewMapFeat& F = feats[fi];
      if (F.layer != want) continue;
      if (nmStyleFloor(F.layer, F.min_scale, F.flags) > (uint16_t)view_scale) continue;
      if (F.count < 2 || F.start + F.count > n_pts) continue;
      LayerStyle st = nmStyle(F.layer, _scale, F.min_scale, F.flags);
      if ((phase == NM_PH_ROAD_CASE || phase == NM_PH_HYDRO_CASE) && st.half_w <= 0)
        continue;

      const bool major = (F.flags & NMF_SIGNED) ||
                         (F.layer >= NML_ROAD_TERTIARY && F.layer <= NML_ROAD_MOTORWAY);

      // Sample along the whole feature so long snaking roads are not dropped
      double fmin_lat = 1e9, fmax_lat = -1e9, fmin_lon = 1e9, fmax_lon = -1e9;
      uint32_t step = major ? 1 : (F.count > 24 ? F.count / 12 : 1);
      if (step < 1) step = 1;
      if (!major && F.count > 80) step = F.count / 20;
      for (uint32_t i = 0; i < F.count; i += step) {
        double la = pts[F.start + i].lat_s / sc;
        double lo = pts[F.start + i].lon_s / sc;
        if (la < fmin_lat) fmin_lat = la;
        if (la > fmax_lat) fmax_lat = la;
        if (lo < fmin_lon) fmin_lon = lo;
        if (lo > fmax_lon) fmax_lon = lo;
      }
      {
        double la = pts[F.start + F.count - 1].lat_s / sc;
        double lo = pts[F.start + F.count - 1].lon_s / sc;
        if (la < fmin_lat) fmin_lat = la;
        if (la > fmax_lat) fmax_lat = la;
        if (lo < fmin_lon) fmin_lon = lo;
        if (lo > fmax_lon) fmax_lon = lo;
      }
      if (fmax_lat < lat0 || fmin_lat > lat1 || fmax_lon < lon0 || fmin_lon > lon1)
        continue;

      int stride = 1;
      if (!major) {
        if (_scale < 64 && F.count > 40) stride = 4;
        else if (_scale < 128 && F.count > 30) stride = 3;
        else if (_scale < 256 && F.count > 24) stride = 2;
      }

      // Closed areas (lakes, parks): sample the whole ring. A huge lake's
      // first 128 verts can all sit off-screen while the water fills the view.
      if (st.fill_poly && (F.flags & NMF_CLOSED) &&
          (phase == NM_PH_AREA || phase == NM_PH_BUILDING) && F.count >= 3) {
        int n = (int)F.count;
        if (n > 128) n = 128;
        for (int k = 0; k < n; k++) {
          uint32_t i = (F.count <= 128) ? (uint32_t)k
                                       : (uint32_t)k * (F.count - 1) / (uint32_t)(n - 1);
          double la = pts[F.start + i].lat_s / sc;
          double lo = pts[F.start + i].lon_s / sc;
          project(la, lo, xs[k], ys_[k]);
        }
        bool covers = (fmin_lat <= lat0 && fmax_lat >= lat1 &&
                       fmin_lon <= lon0 && fmax_lon >= lon1);
        if (covers && want == NML_WATER_AREA) {
          c.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, st.fill);
        } else {
          nmFillPoly(c, xs, ys_, n, st.fill);
          for (int k = 0; k < n; k++) {
            int j = (k + 1) % n;
            c.drawLine(xs[k], ys_[k], xs[j], ys_[j],
                       st.cas != st.fill ? st.cas : st.fill);
          }
        }
        continue;
      }

      // Window long polylines so stitching does not get chopped at 95 verts
      uint32_t i0 = 0;
      while (i0 < F.count) {
        int n = 0;
        bool any_on = false;
        uint32_t i = i0;
        for (; i < F.count && n < MAXV - 1; i += (uint32_t)stride) {
          double la = pts[F.start + i].lat_s / sc;
          double lo = pts[F.start + i].lon_s / sc;
          int x, y;
          project(la, lo, x, y);
          xs[n] = x; ys_[n] = y;
          if (nmOnScreen(x, y)) any_on = true;
          n++;
        }
        uint32_t last_i = (i >= F.count) ? F.count - 1 : i;
        if (last_i > i0) {
          double la = pts[F.start + last_i].lat_s / sc;
          double lo = pts[F.start + last_i].lon_s / sc;
          int x, y;
          project(la, lo, x, y);
          if (n == 0 || xs[n - 1] != x || ys_[n - 1] != y) {
            xs[n] = x; ys_[n] = y; n++;
          }
          if (nmOnScreen(x, y)) any_on = true;
        }
        if (n >= 2 && any_on) {
          bool closed = (F.flags & NMF_CLOSED) != 0 && i0 == 0 && last_i == F.count - 1;
          if (st.fill_poly && closed && n >= 3 && phase == NM_PH_AREA) {
            nmFillPoly(c, xs, ys_, n, st.fill);
            for (int k = 0; k < n; k++) {
              int j = (k + 1) % n;
              c.drawLine(xs[k], ys_[k], xs[j], ys_[j],
                         st.cas != st.fill ? st.cas : st.fill);
            }
          } else if (st.fill_poly && closed && n >= 3 && phase == NM_PH_BUILDING) {
            nmFillPoly(c, xs, ys_, n, st.fill);
            for (int k = 0; k < n; k++) {
              int j = (k + 1) % n;
              c.drawLine(xs[k], ys_[k], xs[j], ys_[j], st.cas);
            }
          } else if (st.dashed) {
            nmPolyDash(c, xs, ys_, n, st.fill);
          } else if (phase == NM_PH_ROAD_CASE || phase == NM_PH_HYDRO_CASE) {
            nmPolyCapsule(c, xs, ys_, n, st.half_w + 1, st.cas);
          } else {
            nmPolyCapsule(c, xs, ys_, n, st.half_w, st.fill);
          }
        }
        if (last_i + 1 >= F.count) break;
        i0 = last_i;  // overlap 1 vertex so windows join
      }
    }
  }
}

void NewMapsScreen::drawAllLists(const NewMapPack* pk, uint8_t phase) {
  if (!pk || !pk->loaded) return;
  drawFeatList(pk, pk->pts, pk->n_points, pk->feats, pk->n_feats, phase);
  if (pk->format == 2) {
    for (int i = 0; i < NEWM_TILE_CACHE; i++) {
      const NewMapTileSlot& s = pk->cache[i];
      if (!s.used || !s.pts || !s.feats) continue;
      drawFeatList(pk, s.pts, s.n_pts, s.feats, s.n_feats, phase);
    }
  }
}

void NewMapsScreen::drawPack(const NewMapPack* pk) {
  if (!pk || !pk->loaded) return;
  drawAllLists(pk, NM_PH_AREA);
  drawAllLists(pk, NM_PH_HYDRO_CASE);
  drawAllLists(pk, NM_PH_HYDRO_FILL);
  drawAllLists(pk, NM_PH_RAIL_PATH);
  drawAllLists(pk, NM_PH_ROAD_CASE);
  drawAllLists(pk, NM_PH_ROAD_FILL);
  drawAllLists(pk, NM_PH_BUILDING);
}

void NewMapsScreen::drawPackLabels(const NewMapPack* pk) {
  if (!pk || !pk->loaded) return;
  // Labels
  if (pk->labels && _scale >= 48) {
    GFXcanvas16& c = ui.cv();
    const float sc = pk->scale;
    const int16_t view_scale = (int16_t)(_scale > 65535 ? 65535 : _scale);
    double ys = _scale / cos(_clat * 0.017453292519943295);
    double half_lon = (SCREEN_W / 2.0) / _scale;
    double half_lat = (SCREEN_H / 2.0) / ys;
    double lat0 = _clat - half_lat, lat1 = _clat + half_lat;
    double lon0 = _clon - half_lon, lon1 = _clon + half_lon;
    c.setTextSize(1);
    for (uint32_t i = 0; i < pk->n_labels; i++) {
      const NewMapLabel& L = pk->labels[i];
      uint16_t need = (uint16_t)L.min_scale_div * 4;
      if (need > (uint16_t)view_scale) continue;
      // kind filtering
      if (L.kind >= 4 && _scale < 768) continue;   // suburb / neighbourhood
      if (L.kind >= 3 && _scale < 384) continue;   // hamlet
      if (L.kind >= 2 && _scale < 192) continue;   // village
      if (L.kind >= 1 && _scale < 48) continue;    // town
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

void NewMapsScreen::captureSnap() {
  if (!_snap) {
    _snap = (uint16_t*)heap_caps_malloc(
        (size_t)SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_snap)
      _snap = (uint16_t*)heap_caps_malloc(
          (size_t)SCREEN_W * SCREEN_H * 2, MALLOC_CAP_8BIT);
  }
  if (!_snap) {
    _snap_ok = false;
    return;
  }
  memcpy(_snap, ui.cv().getBuffer(), (size_t)SCREEN_W * SCREEN_H * 2);
  _snap_clat = _clat;
  _snap_clon = _clon;
  _snap_scale = _scale;
  _snap_ok = true;
}

bool NewMapsScreen::blitPanPreview() {
  if (!_snap_ok || !_snap || _snap_scale != _scale) return false;
  GFXcanvas16& c = ui.cv();
  double ys = _scale / cos(_clat * 0.017453292519943295);
  int dx = (int)lround((_snap_clon - _clon) * _scale);
  int dy = (int)lround((_clat - _snap_clat) * ys);

  c.fillScreen(NM_BG);
  uint16_t* dst = c.getBuffer();
  int src_x0 = dx < 0 ? -dx : 0;
  int src_y0 = dy < 0 ? -dy : 0;
  int dst_x0 = dx > 0 ? dx : 0;
  int dst_y0 = dy > 0 ? dy : 0;
  int w = SCREEN_W - (dx >= 0 ? dx : -dx);
  int h = SCREEN_H - (dy >= 0 ? dy : -dy);
  if (w > 0 && h > 0) {
    for (int y = 0; y < h; y++) {
      memcpy(dst + (size_t)(dst_y0 + y) * SCREEN_W + dst_x0,
             _snap + (size_t)(src_y0 + y) * SCREEN_W + src_x0,
             (size_t)w * 2);
    }
  }
  return true;
}

void NewMapsScreen::drawChrome(bool have) {
  GFXcanvas16& c = ui.cv();

  c.drawFastHLine(SCREEN_W / 2 - 6, SCREEN_H / 2, 13, C_FG_DIM);
  c.drawFastVLine(SCREEN_W / 2, SCREEN_H / 2 - 6, 13, C_FG_DIM);
  c.drawCircle(SCREEN_W / 2, SCREEN_H / 2, 3, C_FG_FAINT);

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
  c.setTextSize(1);
  c.setCursor(12, SCREEN_H - 26);
  c.print(sb);

  char title[40];
  if (_panning)
    snprintf(title, sizeof(title), "NewMaps  z%d", nmZoomIndex(_scale));
  else if (have && _tiles_pending)
    snprintf(title, sizeof(title), "NewMaps  z%d ...", nmZoomIndex(_scale));
  else if (have)
    snprintf(title, sizeof(title), "NewMaps  z%d", nmZoomIndex(_scale));
  else
    snprintf(title, sizeof(title), "NewMaps");
  ui.drawStatusBar(title);

  c.setTextColor(C_FG_FAINT);
  c.setCursor(90, SCREEN_H - 12);
  c.print("long-tap info  +/-");

  c.setTextColor(C_FG_FAINT);
  c.setCursor(SCREEN_W - 92, STATUS_H + 2);
  c.print("c OSM");
}

void NewMapsScreen::draw() {
  GFXcanvas16& c = ui.cv();

  // During a finger-drag, slide the last full frame. A full vector rebuild
  // (and SD tile I/O) on every touch sample is what made swipe feel stuck.
  if (_panning && blitPanPreview()) {
    const NewMapPack* pk = ui.newmaps.pack(_pack_i);
    drawChrome(pk && pk->loaded);
    return;
  }

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

  _pack_i = ui.newmaps.packIndexFor(_clat, _clon);
  int vis[NEWM_MAX_PACKS];
  int nvis = ui.newmaps.packsIntersecting(lat0, lat1, lon0, lon1, vis, NEWM_MAX_PACKS);
  bool have = nvis > 0;
  if (have) {
    bool done = true;
    for (int i = 0; i < nvis; i++) {
      if (!ui.newmaps.ensureTiles(ui.hw, vis[i], lat0, lat1, lon0, lon1, _scale))
        done = false;
    }
    _tiles_pending = !done;
    if (_tiles_pending) ui.requestDraw();
    // Same phase across every region so roads meet at file edges
    static const uint8_t kPh[] = {
      NM_PH_AREA, NM_PH_HYDRO_CASE, NM_PH_HYDRO_FILL, NM_PH_RAIL_PATH,
      NM_PH_ROAD_CASE, NM_PH_ROAD_FILL, NM_PH_BUILDING
    };
    for (unsigned ph = 0; ph < sizeof(kPh); ph++) {
      for (int i = 0; i < nvis; i++) {
        const NewMapPack* pk = ui.newmaps.pack(vis[i]);
        if (pk) drawAllLists(pk, kPh[ph]);
      }
    }
    for (int i = 0; i < nvis; i++)
      drawPackLabels(ui.newmaps.pack(vis[i]));
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
  drawChrome(have);
  captureSnap();
  if (_info_on) drawInfoCard();
}

void NewMapsScreen::screenToLatLon(int x, int y, double& lat, double& lon) const {
  double ys = _scale / cos(_clat * 0.017453292519943295);
  lon = _clon + (double)(x - SCREEN_W / 2) / _scale;
  lat = _clat + (double)(SCREEN_H / 2 - y) / ys;
}

static float nmDistSeg2(int px, int py, int x0, int y0, int x1, int y1) {
  int dx = x1 - x0, dy = y1 - y0;
  int l2 = dx * dx + dy * dy;
  if (l2 < 1) {
    int ex = px - x0, ey = py - y0;
    return (float)(ex * ex + ey * ey);
  }
  float t = (float)((px - x0) * dx + (py - y0) * dy) / (float)l2;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  float ex = (float)x0 + t * (float)dx - (float)px;
  float ey = (float)y0 + t * (float)dy - (float)py;
  return ex * ex + ey * ey;
}

static const char* nmKindName(uint8_t layer, uint8_t flags) {
  if (flags & NMF_DRIVEWAY) return "Driveway";
  switch (layer) {
    case NML_ROAD_MOTORWAY: return "Interstate / motorway";
    case NML_ROAD_TRUNK:    return "US / state highway";
    case NML_ROAD_PRIMARY:  return "US / primary highway";
    case NML_ROAD_SECONDARY:return "State / secondary route";
    case NML_ROAD_TERTIARY: return "Tertiary / collector";
    case NML_ROAD_RESIDENTIAL: return "Residential street";
    case NML_ROAD_SERVICE:  return "Service road";
    case NML_ROAD_PATH:     return "Path / trail";
    case NML_RAIL:          return "Railroad";
    case NML_WATER_LINE:    return "River / stream";
    case NML_WATER_AREA:    return "Lake / water";
    case NML_LANDUSE_PARK:  return "Park";
    case NML_LANDUSE_FOREST:return "Forest";
    default: return "Map feature";
  }
}

struct NmHit {
  float best = 28.f * 28.f;
  uint8_t layer = 0xFF;
  uint8_t flags = 0;
  uint16_t name_id = 0;
  const NewMapPack* pk = nullptr;
};

static void nmScanFeats(const NewMapPack* pk, const NewMapPt* pts, uint32_t n_pts,
                        const NewMapFeat* feats, uint32_t n_feats,
                        const NewMapsScreen* scr, int tx, int ty, NmHit& hit) {
  if (!pk || !pts || !feats) return;
  const float sc = pk->scale;
  for (uint32_t fi = 0; fi < n_feats; fi++) {
    const NewMapFeat& F = feats[fi];
    if (F.layer < NML_ROAD_PATH || F.layer > NML_ROAD_MOTORWAY) continue;
    if (F.count < 2 || F.start + F.count > n_pts) continue;
    int lx = 0, ly = 0;
    bool have = false;
    for (uint32_t i = 0; i < F.count; i++) {
      double la = pts[F.start + i].lat_s / sc;
      double lo = pts[F.start + i].lon_s / sc;
      int x, y;
      scr->project(la, lo, x, y);
      if (have) {
        float d = nmDistSeg2(tx, ty, lx, ly, x, y);
        bool take = false;
        if (d + 4.f < hit.best) {
          take = true;
        } else if (d <= hit.best + 4.f) {
          const bool ns = (F.flags & NMF_SIGNED) != 0;
          const bool os = (hit.flags & NMF_SIGNED) != 0;
          if (ns && !os) take = true;
          else if (ns == os && F.layer > hit.layer) take = true;
          else if (ns == os && F.layer == hit.layer && d < hit.best) take = true;
        }
        if (take) {
          hit.best = d;
          hit.layer = F.layer;
          hit.flags = F.flags;
          hit.name_id = F.name_id;
          hit.pk = pk;
        }
      }
      lx = x; ly = y; have = true;
    }
  }
}

void NewMapsScreen::identifyAt(int x, int y) {
  double tlat, tlon;
  screenToLatLon(x, y, tlat, tlon);
  char ns = tlat >= 0 ? 'N' : 'S';
  char ew = tlon >= 0 ? 'E' : 'W';
  snprintf(_info_coord, sizeof(_info_coord), "%.4f %c  %.4f %c",
           fabs(tlat), ns, fabs(tlon), ew);

  _info_title[0] = 0;
  _info_sub[0] = 0;
  _info_kind[0] = 0;
  _info_place[0] = 0;

  double ys = _scale / cos(_clat * 0.017453292519943295);
  double half_lon = (SCREEN_W / 2.0) / _scale;
  double half_lat = (SCREEN_H / 2.0) / ys;
  int vis[NEWM_MAX_PACKS];
  int nvis = ui.newmaps.packsIntersecting(
      _clat - half_lat, _clat + half_lat, _clon - half_lon, _clon + half_lon,
      vis, NEWM_MAX_PACKS);

  NmHit hit;
  for (int i = 0; i < nvis; i++) {
    const NewMapPack* pk = ui.newmaps.pack(vis[i]);
    if (!pk) continue;
    nmScanFeats(pk, pk->pts, pk->n_points, pk->feats, pk->n_feats,
                this, x, y, hit);
    if (pk->format == 2) {
      for (int t = 0; t < NEWM_TILE_CACHE; t++) {
        const NewMapTileSlot& s = pk->cache[t];
        if (!s.used || !s.pts) continue;
        nmScanFeats(pk, s.pts, s.n_pts, s.feats, s.n_feats,
                    this, x, y, hit);
      }
    }
  }

  const NewMapName* named = nullptr;
  if (hit.pk && hit.name_id && hit.pk->names &&
      hit.name_id <= hit.pk->n_names)
    named = &hit.pk->names[hit.name_id - 1];

  if (hit.layer != 0xFF)
    snprintf(_info_kind, sizeof(_info_kind), "%s", nmKindName(hit.layer, hit.flags));
  else
    snprintf(_info_kind, sizeof(_info_kind), "Map location");

  if (named && (named->ref[0] || named->name[0])) {
    if (named->ref[0])
      snprintf(_info_title, sizeof(_info_title), "%s", named->ref);
    if (named->name[0]) {
      if (_info_title[0] && strcmp(_info_title, named->name) != 0)
        snprintf(_info_sub, sizeof(_info_sub), "%s", named->name);
      else if (!_info_title[0])
        snprintf(_info_title, sizeof(_info_title), "%s", named->name);
    }
    if (named->place[0])
      snprintf(_info_place, sizeof(_info_place), "Near %s", named->place);
  }
  if (!_info_title[0])
    snprintf(_info_title, sizeof(_info_title), "%s",
             hit.layer != 0xFF ? nmKindName(hit.layer, hit.flags) : "Location");

  if (!_info_place[0]) {
    float pbest = 1e12f;
    const char* pname = nullptr;
    for (int i = 0; i < nvis; i++) {
      const NewMapPack* pk = ui.newmaps.pack(vis[i]);
      if (!pk || !pk->labels) continue;
      const float sc = pk->scale;
      for (uint32_t li = 0; li < pk->n_labels; li++) {
        const NewMapLabel& L = pk->labels[li];
        if (L.kind > 2) continue;
        int lx, ly;
        project(L.lat_s / sc, L.lon_s / sc, lx, ly);
        float d = (float)((lx - x) * (lx - x) + (ly - y) * (ly - y));
        d += (float)L.kind * 80.f;
        if (d < pbest) { pbest = d; pname = L.name; }
      }
    }
    if (pname && pbest < 200.f * 200.f)
      snprintf(_info_place, sizeof(_info_place), "Near %s", pname);
  }

  _info_on = true;
  _info_opened = millis();
}

void NewMapsScreen::drawInfoCard() {
  GFXcanvas16& c = ui.cv();
  const int mw = 268, mh = 118;
  const int mx = (SCREEN_W - mw) / 2;
  const int my = SCREEN_H - mh - 10;
  c.fillRoundRect(mx, my, mw, mh, 8, RGB565(255, 255, 250));
  c.drawRoundRect(mx, my, mw, mh, 8, NM_PRI_CASE);
  c.fillRect(mx + 8, my + 8, 4, mh - 16, NM_PRI);
  c.setTextSize(2);
  c.setTextColor(NM_LABEL);
  c.setCursor(mx + 20, my + 12);
  c.print(_info_title);
  c.setTextSize(1);
  int yy = my + 34;
  if (_info_sub[0]) {
    c.setTextColor(NM_PRI_CASE);
    c.setCursor(mx + 20, yy);
    c.print(_info_sub);
    yy += 12;
  }
  c.setTextColor(C_FG_DIM);
  c.setCursor(mx + 20, yy);
  c.print(_info_kind);
  yy += 12;
  if (_info_place[0]) {
    c.setTextColor(NM_LABEL);
    c.setCursor(mx + 20, yy);
    c.print(_info_place);
    yy += 12;
  }
  c.setTextColor(C_FG_DIM);
  c.setCursor(mx + 20, yy);
  c.print(_info_coord);
  c.setTextColor(C_FG_FAINT);
  c.setCursor(mx + 20, my + mh - 16);
  c.print("tap to close");
}

bool NewMapsScreen::zoomBy(int dir) {
  _panning = false;
  _pinching = false;
  int zi = nmZoomIndex(_scale);
  int nzi = zi + dir;
  if (nzi < 0) nzi = 0;
  if (nzi >= NM_N_ZOOMS) nzi = NM_N_ZOOMS - 1;
  _scale = NM_ZOOMS[nzi];
  return true;
}

bool NewMapsScreen::key(uint8_t k) {
  if (_info_on && (k == 0x1B || k == 0x08 || k == 0x7F || k == 0x0D)) {
    _info_on = false;
    return true;
  }
  // T-Deck prints + on O and - on I. Also accept q/a and symbol-layer + -.
  if (k == '+' || k == '=' || k == 'q' || k == 'Q' || k == ']' ||
      k == 'o' || k == 'O')
    return zoomBy(1);
  if (k == '-' || k == '_' || k == 'a' || k == 'A' || k == '[' ||
      k == 'i' || k == 'I')
    return zoomBy(-1);
  if (k == 'c' || k == 'C') {
    _panning = false;
    double lat, lon;
    if (ui.ownPos(lat, lon)) { _clat = lat; _clon = lon; }
    else if (const NewMapPack* p = ui.newmaps.pack(0)) {
      _clat = 0.5 * (p->lat_min + p->lat_max);
      _clon = 0.5 * (p->lon_min + p->lon_max);
    }
    return true;
  }
  if (k == '0') { _panning = false; _scale = NM_ZOOMS[0]; return true; }
  if (k == '9') { _panning = false; _scale = NM_ZOOMS[NM_N_ZOOMS - 1]; return true; }
  return false;
}

bool NewMapsScreen::nav(NavEvent e) {
  if (_info_on) {
    if (e == NAV_BACK || e == NAV_SELECT) { _info_on = false; return true; }
  }
  double ys = _scale / cos(_clat * 0.017453292519943295);
  double dlon = 28.0 / _scale, dlat = 28.0 / ys;
  switch (e) {
    case NAV_UP:    _clat += dlat; if (_clat > 85) _clat = 85; return true;
    case NAV_DOWN:  _clat -= dlat; if (_clat < -85) _clat = -85; return true;
    case NAV_LEFT:  _clon -= dlon; if (_clon < -180) _clon = -180; return true;
    case NAV_RIGHT: _clon += dlon; if (_clon > 180) _clon = 180; return true;
    case NAV_SELECT:
      return zoomBy(1);
    default: return false;
  }
}

bool NewMapsScreen::touch(const TouchEvent& e) {
  if (_info_on) {
    if (e.kind == TouchEvent::LONG) {
      identifyAt(e.x, e.y);
      return true;
    }
    if (e.kind == TouchEvent::RELEASE && millis() - _info_opened < 600)
      return true;  // swallow the lift after long-press
    if (e.kind == TouchEvent::TAP || e.kind == TouchEvent::RELEASE) {
      _info_on = false;
      return true;
    }
    if (e.kind == TouchEvent::DRAG) {
      _info_on = false;
      // fall through to pan
    } else {
      return true;
    }
  }
  if (e.kind == TouchEvent::LONG) {
    identifyAt(e.x, e.y);
    _panning = false;
    return true;
  }
  if (e.kind == TouchEvent::PINCH) {
    _panning = false;
    int dist = e.dy;
    if (dist < 8) dist = 8;
    if (!_pinching) {
      _pinching = true;
      _pinch_anchor = dist;
      return true;
    }
    if (dist > _pinch_anchor + _pinch_anchor / 8 + 6) {
      zoomBy(1);
      _pinching = true;
      _pinch_anchor = dist;
      return true;
    }
    if (dist < _pinch_anchor - _pinch_anchor / 8 - 6) {
      zoomBy(-1);
      _pinching = true;
      _pinch_anchor = dist;
      return true;
    }
    return true;
  }
  if (e.kind == TouchEvent::DRAG) {
    _pinching = false;
    _panning = true;
    double ys = _scale / cos(_clat * 0.017453292519943295);
    _clon -= e.dx / _scale;
    _clat += e.dy / ys;
    if (_clat > 85) _clat = 85;
    if (_clat < -85) _clat = -85;
    if (_clon < -180) _clon = -180;
    if (_clon > 180) _clon = 180;
    return true;
  }
  if (e.kind == TouchEvent::RELEASE) {
    _panning = false;
    _pinching = false;
    // Short lift with little travel: treat as tap (double-tap zoom)
    if (abs(e.dx) < 24 && abs(e.dy) < 24) {
      TouchEvent t = e;
      t.kind = TouchEvent::TAP;
      return touch(t);
    }
    return true;
  }
  if (e.kind == TouchEvent::TAP) {
    uint32_t now = millis();
    bool dbl = (now - _last_tap_ms) < 450 &&
               abs(e.x - _last_tap_x) < 40 &&
               abs(e.y - _last_tap_y) < 40;
    _last_tap_ms = now;
    _last_tap_x = e.x;
    _last_tap_y = e.y;
    if (dbl) {
      _last_tap_ms = 0;
      return zoomBy(1);
    }
    return true;  // single tap does not zoom
  }
  return false;
}
