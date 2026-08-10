#include "NewMap.h"
#include "DeckHW.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

static void* psAlloc(size_t sz) {
  void* p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(sz, MALLOC_CAP_8BIT);
  if (!p) p = malloc(sz);
  return p;
}

void NewMaps::unload() {
  for (int i = 0; i < _n; i++) {
    NewMapPack& p = _packs[i];
    if (p.pts) free(p.pts);
    if (p.feats) free(p.feats);
    if (p.labels) free(p.labels);
    memset(&p, 0, sizeof(p));
  }
  _n = 0;
}

bool NewMaps::loadFile(const char* path) {
  if (_n >= NEWM_MAX_PACKS) return false;
  File f = SD.open(path);
  if (!f) return false;

  uint32_t magic = 0;
  uint16_t ver = 0, flags = 0;
  float bbox[4] = {0};
  float scale = 0;
  uint32_t npts = 0, nfeats = 0, nlabels = 0;

  bool ok = f.read((uint8_t*)&magic, 4) == 4 && magic == NEWM_MAGIC;
  ok = ok && f.read((uint8_t*)&ver, 2) == 2 && ver == NEWM_VERSION;
  ok = ok && f.read((uint8_t*)&flags, 2) == 2;
  ok = ok && f.read((uint8_t*)bbox, 16) == 16;
  ok = ok && f.read((uint8_t*)&scale, 4) == 4;
  ok = ok && f.read((uint8_t*)&npts, 4) == 4;
  ok = ok && f.read((uint8_t*)&nfeats, 4) == 4;
  ok = ok && f.read((uint8_t*)&nlabels, 4) == 4;

  // Sanity: keep packs within PSRAM budget (~Northern ID detail pack)
  if (!ok || scale < 100.f || scale > 1e7f ||
      npts > 800000 || nfeats > 120000 || nlabels > 8000) {
    f.close();
    return false;
  }
  if (bbox[0] >= bbox[1] || bbox[2] >= bbox[3]) {
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
  p->n_points = npts;
  p->n_feats = nfeats;
  p->n_labels = nlabels;

  p->pts = npts ? (NewMapPt*)psAlloc(sizeof(NewMapPt) * npts) : nullptr;
  p->feats = nfeats ? (NewMapFeat*)psAlloc(sizeof(NewMapFeat) * nfeats) : nullptr;
  p->labels = nlabels ? (NewMapLabel*)psAlloc(sizeof(NewMapLabel) * nlabels) : nullptr;
  if ((npts && !p->pts) || (nfeats && !p->feats) || (nlabels && !p->labels)) {
    if (p->pts) free(p->pts);
    if (p->feats) free(p->feats);
    if (p->labels) free(p->labels);
    f.close();
    return false;
  }

  if (npts)
    ok = f.read((uint8_t*)p->pts, sizeof(NewMapPt) * npts) ==
         (int)(sizeof(NewMapPt) * npts);
  if (nfeats)
    ok = ok && f.read((uint8_t*)p->feats, sizeof(NewMapFeat) * nfeats) ==
                   (int)(sizeof(NewMapFeat) * nfeats);
  if (nlabels)
    ok = ok && f.read((uint8_t*)p->labels, sizeof(NewMapLabel) * nlabels) ==
                   (int)(sizeof(NewMapLabel) * nlabels);
  f.close();
  if (!ok) {
    if (p->pts) free(p->pts);
    if (p->feats) free(p->feats);
    if (p->labels) free(p->labels);
    return false;
  }

  // Null-terminate label names
  for (uint32_t i = 0; i < nlabels; i++)
    p->labels[i].name[sizeof(p->labels[i].name) - 1] = 0;

  const char* base = strrchr(path, '/');
  strncpy(p->filename, base ? base + 1 : path, sizeof(p->filename) - 1);
  p->loaded = true;
  _n++;
  Serial.printf("[newmap] loaded %s: %u pts %u feats %u labels (%.1f..%.1f, %.1f..%.1f)\n",
                p->filename, npts, nfeats, nlabels,
                p->lat_min, p->lat_max, p->lon_min, p->lon_max);
  return true;
}

int NewMaps::load(DeckHW& hw) {
  unload();
  if (!hw.sdBegin()) {
    hw.sdEnd();
    return -1;
  }
  File dir = SD.open("/meshdeck-maps");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f && _n < NEWM_MAX_PACKS) {
      const char* nm = f.name();
      size_t l = strlen(nm);
      if (!f.isDirectory() && l > 4 && strcasecmp(nm + l - 4, ".mdv") == 0) {
        char path[96];
        if (nm[0] == '/') snprintf(path, sizeof(path), "%s", nm);
        else snprintf(path, sizeof(path), "/meshdeck-maps/%s", nm);
        f.close();
        loadFile(path);
      } else {
        f.close();
      }
      f = dir.openNextFile();
    }
    dir.close();
  }
  hw.sdEnd();
  return _n;
}

const NewMapPack* NewMaps::packFor(double lat, double lon) const {
  const NewMapPack* best = nullptr;
  for (int i = 0; i < _n; i++) {
    const NewMapPack* p = &_packs[i];
    if (!p->loaded) continue;
    if (lat >= p->lat_min && lat <= p->lat_max &&
        lon >= p->lon_min && lon <= p->lon_max)
      return p;
    if (!best) best = p;
  }
  return best;
}
