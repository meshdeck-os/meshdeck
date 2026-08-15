#include "NewMap.h"
#include "DeckHW.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

static void* psAlloc(size_t sz) {
  void* p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!p) p = malloc(sz);
  return p;
}

void NewMaps::freeSlot(NewMapTileSlot& s) {
  if (s.pts) { free(s.pts); s.pts = nullptr; }
  if (s.feats) { free(s.feats); s.feats = nullptr; }
  s.used = false;
  s.n_pts = 0;
  s.n_feats = 0;
}

void NewMaps::freePack(NewMapPack& p) {
  if (p.pts) free(p.pts);
  if (p.feats) free(p.feats);
  if (p.labels) free(p.labels);
  if (p.names) free(p.names);
  for (int i = 0; i < NEWM_MAX_LODS; i++) {
    if (p.lods[i].index) free(p.lods[i].index);
  }
  for (int i = 0; i < NEWM_TILE_CACHE; i++) freeSlot(p.cache[i]);
  memset(&p, 0, sizeof(p));
}

void NewMaps::unload() {
  for (int i = 0; i < _n; i++) freePack(_packs[i]);
  _n = 0;
}

void NewMaps::releaseTiles(int pack_i) {
  if (pack_i < 0 || pack_i >= _n) return;
  NewMapPack& p = _packs[pack_i];
  for (int i = 0; i < NEWM_TILE_CACHE; i++) freeSlot(p.cache[i]);
}

static bool readExact(File& f, void* dst, size_t n) {
  return f.read((uint8_t*)dst, n) == (int)n;
}

// Disk feat size: MDV1 / MDV2 v2 = 12 B; MDV2 v3+ = 14 B (name_id).
static uint8_t featDiskSize(uint16_t mdv_ver) {
  return mdv_ver >= NEWM2_VERSION_NAMED ? (uint8_t)sizeof(NewMapFeat) : 12;
}

static bool readFeatArray(File& f, NewMapFeat* dst, uint32_t n, uint16_t mdv_ver) {
  if (!n) return true;
  if (!dst) return false;
  const uint8_t dsz = featDiskSize(mdv_ver);
  if (dsz == sizeof(NewMapFeat))
    return readExact(f, dst, sizeof(NewMapFeat) * n);
  for (uint32_t i = 0; i < n; i++) {
    uint8_t buf[12];
    if (!readExact(f, buf, 12)) return false;
    dst[i].layer = buf[0];
    dst[i].flags = buf[1];
    memcpy(&dst[i].min_scale, buf + 2, 2);
    memcpy(&dst[i].start, buf + 4, 4);
    memcpy(&dst[i].count, buf + 8, 4);
    dst[i].name_id = 0;
  }
  return true;
}

bool NewMaps::loadMdv2(void* filep, NewMapPack* p) {
  File& f = *static_cast<File*>(filep);

  uint8_t n_lods = 0, reserved0 = 0;
  uint16_t nlabels = 0;
  uint32_t labels_off = 0, ov_off = 0, ov_bytes = 0, ov_pts = 0, ov_feats = 0;
  uint32_t r1 = 0, r2 = 0;
  uint8_t pad[4];

  if (!readExact(f, &n_lods, 1) || !readExact(f, &reserved0, 1) ||
      !readExact(f, &nlabels, 2) ||
      !readExact(f, &labels_off, 4) || !readExact(f, &ov_off, 4) ||
      !readExact(f, &ov_bytes, 4) || !readExact(f, &ov_pts, 4) ||
      !readExact(f, &ov_feats, 4) || !readExact(f, &r1, 4) ||
      !readExact(f, &r2, 4) || !readExact(f, pad, 4))
    return false;

  if (n_lods == 0 || n_lods > NEWM_MAX_LODS ||
      ov_pts > 150000 || ov_feats > 40000 || nlabels > 8000)
    return false;

  p->format = 2;
  p->n_lods = n_lods;
  p->n_points = ov_pts;
  p->n_feats = ov_feats;
  p->n_labels = nlabels;

  for (uint8_t i = 0; i < n_lods; i++) {
    uint16_t min_s = 0, tx = 0, ty = 0, pad16 = 0;
    float olon = 0, olat = 0, tw = 0, th = 0;
    uint32_t ioff = 0, rsv = 0;
    if (!readExact(f, &min_s, 2) || !readExact(f, &tx, 2) ||
        !readExact(f, &ty, 2) || !readExact(f, &pad16, 2) ||
        !readExact(f, &olon, 4) || !readExact(f, &olat, 4) ||
        !readExact(f, &tw, 4) || !readExact(f, &th, 4) ||
        !readExact(f, &ioff, 4) || !readExact(f, &rsv, 4))
      return false;
    uint32_t nidx = (uint32_t)tx * (uint32_t)ty;
    if (nidx > 8192) return false;
    NewMapLod& L = p->lods[i];
    L.min_scale = min_s;
    L.tiles_x = tx;
    L.tiles_y = ty;
    L.origin_lon = olon;
    L.origin_lat = olat;
    L.tile_w = tw;
    L.tile_h = th;
    L.index_off = ioff;
    L.index = nullptr;
    if (nidx) {
      L.index = (NewMapTileIdx*)psAlloc(sizeof(NewMapTileIdx) * nidx);
      if (!L.index) return false;
    }
  }

  for (uint8_t i = 0; i < n_lods; i++) {
    NewMapLod& L = p->lods[i];
    uint32_t nidx = (uint32_t)L.tiles_x * (uint32_t)L.tiles_y;
    if (!nidx) continue;
    if (!f.seek(L.index_off)) return false;
    if (!readExact(f, L.index, sizeof(NewMapTileIdx) * nidx)) return false;
  }

  if (ov_pts) {
    p->pts = (NewMapPt*)psAlloc(sizeof(NewMapPt) * ov_pts);
    if (!p->pts) return false;
  }
  if (ov_feats) {
    p->feats = (NewMapFeat*)psAlloc(sizeof(NewMapFeat) * ov_feats);
    if (!p->feats) return false;
  }
  if (nlabels) {
    p->labels = (NewMapLabel*)psAlloc(sizeof(NewMapLabel) * nlabels);
    if (!p->labels) return false;
  }

  if (ov_pts || ov_feats) {
    if (!f.seek(ov_off)) return false;
    if (ov_pts && !readExact(f, p->pts, sizeof(NewMapPt) * ov_pts)) return false;
    if (ov_feats && !readFeatArray(f, p->feats, ov_feats, p->mdv_ver))
      return false;
  }
  if (nlabels) {
    if (!f.seek(labels_off)) return false;
    if (!readExact(f, p->labels, sizeof(NewMapLabel) * nlabels)) return false;
    for (uint32_t i = 0; i < nlabels; i++)
      p->labels[i].name[sizeof(p->labels[i].name) - 1] = 0;
  }
  uint32_t names_off = r1, nnames = r2;
  if (nnames > 8000) nnames = 0;
  p->n_names = nnames;
  p->names = nullptr;
  if (nnames && names_off) {
    p->names = (NewMapName*)psAlloc(sizeof(NewMapName) * nnames);
    if (!p->names) {
      p->n_names = 0;
    } else if (!f.seek(names_off) ||
               !readExact(f, p->names, sizeof(NewMapName) * nnames)) {
      free(p->names);
      p->names = nullptr;
      p->n_names = 0;
    } else {
      for (uint32_t i = 0; i < nnames; i++) {
        p->names[i].name[sizeof(p->names[i].name) - 1] = 0;
        p->names[i].ref[sizeof(p->names[i].ref) - 1] = 0;
        p->names[i].place[sizeof(p->names[i].place) - 1] = 0;
      }
    }
  }
  return true;
}

bool NewMaps::loadFile(const char* path) {
  if (_n >= NEWM_MAX_PACKS) return false;
  File f = SD.open(path);
  if (!f) return false;

  uint32_t magic = 0;
  uint16_t ver = 0, flags = 0;
  float bbox[4] = {0};
  float scale = 0;

  bool ok = readExact(f, &magic, 4);
  ok = ok && readExact(f, &ver, 2) && readExact(f, &flags, 2);
  ok = ok && readExact(f, bbox, 16) && readExact(f, &scale, 4);
  if (!ok || scale < 100.f || scale > 1e7f ||
      bbox[0] >= bbox[1] || bbox[2] >= bbox[3]) {
    f.close();
    return false;
  }

  NewMapPack* p = &_packs[_n];
  memset(p, 0, sizeof(*p));
  p->lat_min = bbox[0];
  p->lat_max = bbox[1];
  p->lon_min = bbox[2];
  p->lon_max = bbox[3];
  p->scale = scale;
  strncpy(p->path, path, sizeof(p->path) - 1);

  if (magic == NEWM2_MAGIC && ver >= NEWM2_VERSION && ver <= NEWM2_VERSION_NAMED) {
    p->mdv_ver = ver;
    ok = loadMdv2(&f, p);
    f.close();
    if (!ok) {
      freePack(*p);
      return false;
    }
  } else if (magic == NEWM_MAGIC && ver == NEWM_VERSION) {
    uint32_t npts = 0, nfeats = 0, nlabels = 0;
    ok = readExact(f, &npts, 4) && readExact(f, &nfeats, 4) &&
         readExact(f, &nlabels, 4);
    if (!ok || npts > 800000 || nfeats > 120000 || nlabels > 8000) {
      f.close();
      return false;
    }
    p->format = 1;
    p->n_points = npts;
    p->n_feats = nfeats;
    p->n_labels = nlabels;
    p->pts = npts ? (NewMapPt*)psAlloc(sizeof(NewMapPt) * npts) : nullptr;
    p->feats = nfeats ? (NewMapFeat*)psAlloc(sizeof(NewMapFeat) * nfeats) : nullptr;
    p->labels = nlabels ? (NewMapLabel*)psAlloc(sizeof(NewMapLabel) * nlabels) : nullptr;
    if ((npts && !p->pts) || (nfeats && !p->feats) || (nlabels && !p->labels)) {
      f.close();
      freePack(*p);
      return false;
    }
    if (npts) ok = readExact(f, p->pts, sizeof(NewMapPt) * npts);
    if (nfeats) ok = ok && readFeatArray(f, p->feats, nfeats, /*mdv1*/ 1);
    if (nlabels) ok = ok && readExact(f, p->labels, sizeof(NewMapLabel) * nlabels);
    f.close();
    if (!ok) {
      freePack(*p);
      return false;
    }
    for (uint32_t i = 0; i < nlabels; i++)
      p->labels[i].name[sizeof(p->labels[i].name) - 1] = 0;
  } else {
    f.close();
    return false;
  }

  const char* base = strrchr(path, '/');
  strncpy(p->filename, base ? base + 1 : path, sizeof(p->filename) - 1);
  p->loaded = true;
  _n++;
  Serial.printf("[newmap] loaded %s v%u.%u: %u pts %u feats %u labels %u names (%.1f..%.1f, %.1f..%.1f)\n",
                p->filename, (unsigned)p->format, (unsigned)p->mdv_ver,
                p->n_points, p->n_feats, p->n_labels, p->n_names,
                p->lat_min, p->lat_max, p->lon_min, p->lon_max);
  return true;
}

int NewMaps::load(DeckHW& hw) {
  unload();
  if (!hw.sdBegin()) {
    hw.sdEnd();
    return -1;
  }
  // Prefer MDV2 region files. A leftover giant MDV1 (800k cap) would
  // eat RAM and hide the split packs if we loaded it first.
  char paths[NEWM_MAX_PACKS][96];
  uint8_t vers[NEWM_MAX_PACKS];
  int np = 0;
  File dir = SD.open("/meshdeck-maps");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f && np < NEWM_MAX_PACKS) {
      const char* nm = f.name();
      size_t l = strlen(nm);
      if (!f.isDirectory() && l > 4 && strcasecmp(nm + l - 4, ".mdv") == 0) {
        char path[96];
        if (nm[0] == '/') snprintf(path, sizeof(path), "%s", nm);
        else snprintf(path, sizeof(path), "/meshdeck-maps/%s", nm);
        f.close();
        File peek = SD.open(path);
        uint32_t magic = 0;
        if (peek) {
          peek.read((uint8_t*)&magic, 4);
          peek.close();
        }
        strncpy(paths[np], path, 95);
        paths[np][95] = 0;
        vers[np] = (magic == NEWM2_MAGIC) ? 2 : 1;
        np++;
      } else {
        f.close();
      }
      f = dir.openNextFile();
    }
    dir.close();
  }
  int n2 = 0;
  for (int i = 0; i < np; i++)
    if (vers[i] == 2) n2++;
  for (int i = 0; i < np && _n < NEWM_MAX_PACKS; i++) {
    if (n2 > 0 && vers[i] != 2) continue;  // skip stale MDV1 when MDV2 exists
    loadFile(paths[i]);
  }
  hw.sdEnd();
  return _n;
}

int NewMaps::packIndexFor(double lat, double lon) const {
  int best = -1;
  for (int i = 0; i < _n; i++) {
    const NewMapPack* p = &_packs[i];
    if (!p->loaded) continue;
    if (lat >= p->lat_min && lat <= p->lat_max &&
        lon >= p->lon_min && lon <= p->lon_max)
      return i;
    if (best < 0) best = i;
  }
  return best;
}

const NewMapPack* NewMaps::packFor(double lat, double lon) const {
  int i = packIndexFor(lat, lon);
  return i >= 0 ? &_packs[i] : nullptr;
}

bool NewMaps::packIntersects(int i, double lat0, double lat1,
                             double lon0, double lon1) const {
  if (i < 0 || i >= _n) return false;
  const NewMapPack* p = &_packs[i];
  if (!p->loaded) return false;
  return !(p->lat_max < lat0 || p->lat_min > lat1 ||
           p->lon_max < lon0 || p->lon_min > lon1);
}

int NewMaps::packsIntersecting(double lat0, double lat1, double lon0, double lon1,
                               int* out, int max_out) const {
  int n = 0;
  for (int i = 0; i < _n && n < max_out; i++) {
    if (packIntersects(i, lat0, lat1, lon0, lon1))
      out[n++] = i;
  }
  return n;
}

void NewMaps::releaseAllTiles() {
  for (int i = 0; i < _n; i++) releaseTiles(i);
}

bool NewMaps::readTile(NewMapPack& p, uint8_t lod, uint16_t tx, uint16_t ty,
                       NewMapTileSlot& slot) {
  if (lod >= p.n_lods) return false;
  const NewMapLod& L = p.lods[lod];
  if (tx >= L.tiles_x || ty >= L.tiles_y || !L.index) return false;
  const NewMapTileIdx& e = L.index[(uint32_t)ty * L.tiles_x + tx];
  slot.lod = lod;
  slot.tx = tx;
  slot.ty = ty;
  slot.last_used = _tick;
  slot.used = true;
  slot.n_pts = 0;
  slot.n_feats = 0;
  slot.pts = nullptr;
  slot.feats = nullptr;
  if (e.nbytes == 0 || e.n_pts == 0 || e.n_feats == 0) return true;

  File f = SD.open(p.path);
  if (!f) return false;
  if (!f.seek(e.off)) { f.close(); return false; }

  NewMapPt* pts = (NewMapPt*)psAlloc(sizeof(NewMapPt) * e.n_pts);
  NewMapFeat* feats = (NewMapFeat*)psAlloc(sizeof(NewMapFeat) * e.n_feats);
  bool ok = pts && feats &&
            readExact(f, pts, sizeof(NewMapPt) * e.n_pts) &&
            readFeatArray(f, feats, e.n_feats, p.mdv_ver);
  f.close();
  if (!ok) {
    if (pts) free(pts);
    if (feats) free(feats);
    slot.used = false;
    return false;
  }
  slot.pts = pts;
  slot.feats = feats;
  slot.n_pts = e.n_pts;
  slot.n_feats = e.n_feats;
  return true;
}

bool NewMaps::ensureTiles(DeckHW& hw, int pack_i,
                          double lat0, double lat1, double lon0, double lon1,
                          float view_scale) {
  if (pack_i < 0 || pack_i >= _n) return true;
  NewMapPack& p = _packs[pack_i];
  if (!p.loaded || p.format != 2) return true;

  _tick++;

  struct Want { uint8_t lod; uint16_t tx, ty; };
  Want want[NEWM_TILE_CACHE];
  int nwant = 0;

  auto addWant = [&](uint8_t lod, uint16_t tx, uint16_t ty) {
    if (nwant >= NEWM_TILE_CACHE) return;
    for (int i = 0; i < nwant; i++)
      if (want[i].lod == lod && want[i].tx == tx && want[i].ty == ty) return;
    want[nwant].lod = lod;
    want[nwant].tx = tx;
    want[nwant].ty = ty;
    nwant++;
  };

  // Active LOD + the next-coarser tiled LOD (never page LOD0; it is RAM).
  // Firmware floors beat stale pack lod.min_scale (old packs said 96 / 192).
  static const uint16_t kLodFloor[NEWM_MAX_LODS] = { 0, NM_Z_LOD1, NM_Z_LOD2 };
  int hi = 0;
  for (int i = 1; i < p.n_lods; i++) {
    uint16_t need = p.lods[i].min_scale;
    if (kLodFloor[i] > need) need = kLodFloor[i];
    if (p.lods[i].tiles_x && view_scale + 0.5f >= (float)need)
      hi = i;
  }
  int lo = (hi >= 2) ? hi - 1 : hi;
  if (hi < 1) {
    // Zoomed out: overview only. Drop any leftover tiles to free PSRAM.
    for (int i = 0; i < NEWM_TILE_CACHE; i++)
      if (p.cache[i].used) freeSlot(p.cache[i]);
    return true;
  }

  // Coarser tiled LOD first (street grid), then closer detail if cache remains.
  for (int lod = lo; lod <= hi; lod++) {
    const NewMapLod& L = p.lods[lod];
    if (!L.tiles_x || !L.tiles_y || L.tile_w <= 0 || L.tile_h <= 0) continue;
    auto txOf = [&](double lon) -> int {
      int t = (int)floor((lon - L.origin_lon) / L.tile_w);
      if (t < 0) t = 0;
      if (t >= L.tiles_x) t = L.tiles_x - 1;
      return t;
    };
    auto tyOf = [&](double lat) -> int {
      int t = (int)floor((lat - L.origin_lat) / L.tile_h);
      if (t < 0) t = 0;
      if (t >= L.tiles_y) t = L.tiles_y - 1;
      return t;
    };
    int x0 = txOf(lon0), x1 = txOf(lon1);
    int y0 = tyOf(lat0), y1 = tyOf(lat1);
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    // Visible first, then a 1-tile ring if cache remains.
    for (int pass = 0; pass < 2 && nwant < NEWM_TILE_CACHE; pass++) {
      int px0 = x0, px1 = x1, py0 = y0, py1 = y1;
      if (pass == 1) {
        if (px0 > 0) px0--;
        if (py0 > 0) py0--;
        if (px1 + 1 < L.tiles_x) px1++;
        if (py1 + 1 < L.tiles_y) py1++;
      }
      // Center-out so the cache prefers the view center when we have to drop.
      int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
      for (int rad = 0; rad < L.tiles_x + L.tiles_y && nwant < NEWM_TILE_CACHE; rad++) {
        for (int ty = py0; ty <= py1 && nwant < NEWM_TILE_CACHE; ty++) {
          for (int tx = px0; tx <= px1 && nwant < NEWM_TILE_CACHE; tx++) {
            int d = abs(tx - cx) + abs(ty - cy);
            if (d != rad) continue;
            addWant((uint8_t)lod, (uint16_t)tx, (uint16_t)ty);
          }
        }
      }
    }
  }

  bool keep[NEWM_TILE_CACHE] = {false};
  for (int w = 0; w < nwant; w++) {
    for (int i = 0; i < NEWM_TILE_CACHE; i++) {
      NewMapTileSlot& s = p.cache[i];
      if (s.used && s.lod == want[w].lod && s.tx == want[w].tx && s.ty == want[w].ty) {
        s.last_used = _tick;
        keep[i] = true;
        break;
      }
    }
  }
  for (int i = 0; i < NEWM_TILE_CACHE; i++) {
    if (p.cache[i].used && !keep[i]) freeSlot(p.cache[i]);
  }

  int missing = 0;
  for (int w = 0; w < nwant; w++) {
    bool have = false;
    for (int i = 0; i < NEWM_TILE_CACHE; i++) {
      NewMapTileSlot& s = p.cache[i];
      if (s.used && s.lod == want[w].lod && s.tx == want[w].tx && s.ty == want[w].ty) {
        have = true;
        break;
      }
    }
    if (!have) missing++;
  }
  if (!missing) return true;

  if (!hw.sdBegin()) {
    hw.sdEnd();
    return true;  // no card: keep overview, do not spin redraws
  }
  int loaded = 0;
  for (int w = 0; w < nwant && loaded < NEWM_MAX_LOAD_PER_FRAME; w++) {
    bool have = false;
    int free_i = -1;
    for (int i = 0; i < NEWM_TILE_CACHE; i++) {
      NewMapTileSlot& s = p.cache[i];
      if (s.used && s.lod == want[w].lod && s.tx == want[w].tx && s.ty == want[w].ty) {
        have = true;
        break;
      }
      if (!s.used && free_i < 0) free_i = i;
    }
    if (have || free_i < 0) continue;
    if (readTile(p, want[w].lod, want[w].tx, want[w].ty, p.cache[free_i]))
      loaded++;
    else
      p.cache[free_i].used = false;
  }
  hw.sdEnd();
  if (loaded == 0) return true;

  // Still missing?
  for (int w = 0; w < nwant; w++) {
    bool have = false;
    for (int i = 0; i < NEWM_TILE_CACHE; i++) {
      NewMapTileSlot& s = p.cache[i];
      if (s.used && s.lod == want[w].lod && s.tx == want[w].tx && s.ty == want[w].ty) {
        have = true;
        break;
      }
    }
    if (!have) return false;
  }
  return true;
}
