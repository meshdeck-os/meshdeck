#pragma once
/*
 * MeshDeck NewMaps - multi-layer vector basemap packs (.mdv).
 *
 * Built offline from OpenStreetMap (see tools/gen_newmap.py).
 * Drop .mdv files into /meshdeck-maps/ on the SD card.
 *
 * Format (little-endian) MDV1:
 *   u32 magic 'MDV1'
 *   u16 version (=1)
 *   u16 flags
 *   f32 lat_min, lat_max, lon_min, lon_max
 *   f32 scale          // stored coords = degrees * scale (int32)
 *   u32 n_points
 *   u32 n_feats
 *   u32 n_labels
 *   n_points x { i32 lat_s, i32 lon_s }
 *   n_feats  x { u8 layer, u8 flags, u16 min_scale, u32 start, u32 count }
 *   n_labels x { i32 lat_s, i32 lon_s, u8 kind, u8 min_scale_div, char name[20] }
 *
 * flags bit0 = closed polygon (draw as fill when possible / outline)
 * min_scale  = minimum MapScreen-style px/degree scale to show feature
 */
#include <Arduino.h>

#define NEWM_MAGIC   0x3156444Du   // "MDV1"
#define NEWM_VERSION 1
#define NEWM_MAX_PACKS 2

// Drawing layers (draw order = ascending id for most, roads reordered in screen)
enum NewMapLayer : uint8_t {
  NML_WATER_AREA = 0,
  NML_LANDUSE_PARK,
  NML_LANDUSE_FOREST,
  NML_LANDUSE_RESIDENTIAL,
  NML_LANDUSE_INDUSTRIAL,
  NML_WATER_LINE,
  NML_RAIL,
  NML_ROAD_PATH,
  NML_ROAD_SERVICE,
  NML_ROAD_RESIDENTIAL,
  NML_ROAD_TERTIARY,
  NML_ROAD_SECONDARY,
  NML_ROAD_PRIMARY,
  NML_ROAD_TRUNK,
  NML_ROAD_MOTORWAY,
  NML_BUILDING,
  NML_COUNT
};

#define NMF_CLOSED 0x01

struct NewMapPt {
  int32_t lat_s;
  int32_t lon_s;
} __attribute__((packed));

struct NewMapFeat {
  uint8_t  layer;
  uint8_t  flags;
  uint16_t min_scale;   // show when view scale >= this
  uint32_t start;
  uint32_t count;
} __attribute__((packed));

struct NewMapLabel {
  int32_t lat_s;
  int32_t lon_s;
  uint8_t kind;         // 0=city 1=town 2=village 3=hamlet 4=suburb
  uint8_t min_scale_div; // min_scale stored as scale/4 (0..255 -> 0..1020)
  char    name[20];
} __attribute__((packed));

struct NewMapPack {
  bool loaded = false;
  char filename[40] = {0};
  float lat_min = 0, lat_max = 0, lon_min = 0, lon_max = 0;
  float scale = 1;
  uint32_t n_points = 0, n_feats = 0, n_labels = 0;
  NewMapPt*    pts = nullptr;
  NewMapFeat*  feats = nullptr;
  NewMapLabel* labels = nullptr;
};

class DeckHW;

class NewMaps {
public:
  int load(DeckHW& hw);
  void unload();
  int count() const { return _n; }
  const NewMapPack* pack(int i) const {
    return (i >= 0 && i < _n) ? &_packs[i] : nullptr;
  }
  // Prefer pack covering (lat,lon); else first loaded
  const NewMapPack* packFor(double lat, double lon) const;

private:
  bool loadFile(const char* path);
  NewMapPack _packs[NEWM_MAX_PACKS];
  int _n = 0;
};
