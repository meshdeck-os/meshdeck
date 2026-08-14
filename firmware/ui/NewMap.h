#pragma once
/*
 * MeshDeck NewMaps - multi-layer vector basemap packs (.mdv).
 *
 * Built offline from OpenStreetMap (see tools/gen_newmap.py).
 * Drop .mdv files into /meshdeck-maps/ on the SD card.
 *
 * MDV1 (legacy, load-all):
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
 * MDV2 (tiled LOD — preferred):
 *   64-byte header 'MDV2' + 3x 32-byte LOD records
 *   LOD0 = overview blob always kept in PSRAM (stitched major roads + water)
 *   LOD1/2 = spatial tiles paged from SD for the current view only
 *   Tile index: u32 off, u32 nbytes, u16 n_pts, u16 n_feats
 *   Tile / overview blob: pts then feats (same NewMapFeat layout, start relative)
 *
 * flags bit0 = closed polygon
 * min_scale  = minimum px/degree scale to show feature
 *
 * SD shares SPI with the display: tile I/O must sdBegin / read / sdEnd
 * BEFORE painting the canvas.
 */
#include <Arduino.h>

#define NEWM_MAGIC    0x3156444Du   // "MDV1"
#define NEWM2_MAGIC   0x3256444Du   // "MDV2"
#define NEWM_VERSION  1
#define NEWM2_VERSION 2
#define NEWM_MAX_PACKS 2
#define NEWM_MAX_LODS  3
#define NEWM_TILE_CACHE 9
#define NEWM_MAX_LOAD_PER_FRAME 2

// Client zoom floors (px / deg lon). Pack min_scale may be lower; the
// renderer and tile pager use the stricter of the two so zoomed-out
// views stay light. Scale 256 ~= 90 km across the T-Deck screen.
#define NM_Z_MOTORWAY   8
#define NM_Z_PRIMARY    16
#define NM_Z_SECONDARY  16
#define NM_Z_TERTIARY   24
#define NM_Z_RESIDENT   512
#define NM_Z_SERVICE    1024
#define NM_Z_DRIVEWAY   2048
#define NM_Z_PATH       1536
#define NM_Z_RAIL       96
#define NM_Z_RIVER      16
#define NM_Z_CANAL      64
#define NM_Z_STREAM     512
#define NM_Z_BUILDING   1536
#define NM_Z_LANDUSE    384
#define NM_Z_PARK       96
#define NM_Z_WATER_AREA 16
#define NM_Z_LOD1       512    // do not page residential tiles before this
#define NM_Z_LOD2       1024   // service / path / buildings

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

#define NMF_CLOSED   0x01
#define NMF_DRIVEWAY 0x02   // service=driveway/alley/parking (thinner than service)

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

struct NewMapTileIdx {
  uint32_t off;
  uint32_t nbytes;
  uint16_t n_pts;
  uint16_t n_feats;
} __attribute__((packed));

struct NewMapLod {
  uint16_t min_scale = 0;
  uint16_t tiles_x = 0, tiles_y = 0;
  float origin_lon = 0, origin_lat = 0;
  float tile_w = 0, tile_h = 0;
  uint32_t index_off = 0;
  NewMapTileIdx* index = nullptr;
};

struct NewMapTileSlot {
  bool used = false;
  uint8_t  lod = 0;
  uint16_t tx = 0, ty = 0;
  uint32_t last_used = 0;
  uint16_t n_pts = 0, n_feats = 0;
  NewMapPt*   pts = nullptr;
  NewMapFeat* feats = nullptr;
};

struct NewMapPack {
  bool loaded = false;
  uint8_t format = 1;          // 1 = MDV1, 2 = MDV2
  char filename[40] = {0};
  char path[96] = {0};
  float lat_min = 0, lat_max = 0, lon_min = 0, lon_max = 0;
  float scale = 1;
  uint32_t n_points = 0, n_feats = 0, n_labels = 0;
  NewMapPt*    pts = nullptr;     // MDV1 all pts, or MDV2 overview pts
  NewMapFeat*  feats = nullptr;
  NewMapLabel* labels = nullptr;
  uint8_t n_lods = 0;
  NewMapLod lods[NEWM_MAX_LODS];
  NewMapTileSlot cache[NEWM_TILE_CACHE];
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
  int packIndexFor(double lat, double lon) const;
  // Prefer pack covering (lat,lon); else first loaded
  const NewMapPack* packFor(double lat, double lon) const;

  // Page visible tiles for a MDV2 pack. Call while the display SPI is idle
  // (start of NewMapsScreen::draw is fine: canvas is in PSRAM).
  // Returns false if more tiles still need to be fetched (redraw again).
  bool ensureTiles(DeckHW& hw, int pack_i,
                   double lat0, double lat1, double lon0, double lon1,
                   float view_scale);
  void releaseTiles(int pack_i);

private:
  bool loadFile(const char* path);
  bool loadMdv2(void* file, NewMapPack* p);
  void freePack(NewMapPack& p);
  void freeSlot(NewMapTileSlot& s);
  bool readTile(NewMapPack& p, uint8_t lod, uint16_t tx, uint16_t ty,
                NewMapTileSlot& slot);

  NewMapPack _packs[NEWM_MAX_PACKS];
  int _n = 0;
  uint32_t _tick = 1;
};
