#include "SDMap.h"
#include "DeckHW.h"
#include <SD.h>
#include <esp_heap_caps.h>

static void* psAlloc(size_t sz) {
  void* p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!p) p = malloc(sz);
  return p;
}

bool SDMaps::loadFile(const char* path) {
  if (_n >= SDMAP_MAX_PACKS) return false;
  File f = SD.open(path);
  if (!f) return false;

  uint32_t magic = 0;
  float scale = 0;
  int16_t bbox[4];
  uint32_t npts = 0, nlines = 0, ncities = 0;
  bool ok = f.read((uint8_t*)&magic, 4) == 4 && magic == SDMAP_MAGIC;
  ok = ok && f.read((uint8_t*)&scale, 4) == 4;
  ok = ok && f.read((uint8_t*)bbox, 8) == 8;
  ok = ok && f.read((uint8_t*)&npts, 4) == 4;
  ok = ok && f.read((uint8_t*)&nlines, 4) == 4;
  ok = ok && f.read((uint8_t*)&ncities, 4) == 4;
  // sanity limits: <=200k points, <=20k lines, <=2k cities
  if (!ok || npts > 200000 || nlines > 20000 || ncities > 2000 || scale < 50 || scale > 2000) {
    f.close();
    return false;
  }

  SDMapPack* p = &_packs[_n];
  memset(p, 0, sizeof(*p));
  p->scale = scale;
  p->lat_min = bbox[0] / 100.0f;
  p->lat_max = bbox[1] / 100.0f;
  p->lon_min = bbox[2] / 100.0f;
  p->lon_max = bbox[3] / 100.0f;
  p->npts = npts;
  p->nlines = nlines;
  p->ncities = ncities;
  p->pts = (SDMapPt*)psAlloc(sizeof(SDMapPt) * npts);
  p->lines = (SDMapLine*)psAlloc(sizeof(SDMapLine) * nlines);
  p->cities = ncities ? (SDMapCity*)psAlloc(sizeof(SDMapCity) * ncities) : nullptr;
  if (!p->pts || !p->lines || (ncities && !p->cities)) {
    if (p->pts) free(p->pts);
    if (p->lines) free(p->lines);
    if (p->cities) free(p->cities);
    f.close();
    return false;
  }

  ok = f.read((uint8_t*)p->pts, sizeof(SDMapPt) * npts) == (int)(sizeof(SDMapPt) * npts);
  ok = ok && f.read((uint8_t*)p->lines, sizeof(SDMapLine) * nlines) == (int)(sizeof(SDMapLine) * nlines);
  if (ncities) {
    ok = ok && f.read((uint8_t*)p->cities, sizeof(SDMapCity) * ncities) == (int)(sizeof(SDMapCity) * ncities);
  }
  f.close();
  if (!ok) {
    free(p->pts);
    free(p->lines);
    if (p->cities) free(p->cities);
    return false;
  }

  const char* base = strrchr(path, '/');
  strncpy(p->filename, base ? base + 1 : path, sizeof(p->filename) - 1);
  p->loaded = true;
  _n++;
  return true;
}

int SDMaps::load(DeckHW& hw) {
  _n = 0;
  if (!hw.sdBegin()) {
    hw.sdEnd();
    return -1;   // no card
  }
  File dir = SD.open("/meshdeck-maps");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f && _n < SDMAP_MAX_PACKS) {
      const char* nm = f.name();
      size_t l = strlen(nm);
      if (!f.isDirectory() && l > 4 && strcasecmp(nm + l - 4, ".mdm") == 0) {
        char path[80];
        if (nm[0] == '/') snprintf(path, sizeof(path), "%s", nm);            // name() gave full path
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
