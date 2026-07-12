#pragma once
/*
 * MeshDeck SD map packs.
 * Drop .mdm files (made with tools/gen_sdmap.py) into /meshdeck-maps/ on a
 * FAT32 SD card; they are loaded into PSRAM at boot and drawn by the map
 * screen as high-detail overlays for their region.
 *
 * .mdm format (little endian):
 *   u32 magic 'MDM1'  f32 scale
 *   i16 lat_min*100, lat_max*100, lon_min*100, lon_max*100
 *   u32 npts, u32 nlines, u32 ncities
 *   npts    x { i16 lat*scale, i16 lon*scale }
 *   nlines  x { u32 start, u16 count }
 *   ncities x { i16 lat*100, i16 lon*100, u8 rank, char name[15] }
 */
#include <Arduino.h>

#define SDMAP_MAGIC 0x314D444D
#define SDMAP_MAX_PACKS 4

struct SDMapPt   { int16_t lat, lon; };
struct SDMapLine { uint32_t start; uint16_t count; } __attribute__((packed));
struct SDMapCity { int16_t lat100, lon100; uint8_t rank; char name[15]; } __attribute__((packed));

struct SDMapPack {
  bool loaded;
  char filename[32];
  float scale;
  float lat_min, lat_max, lon_min, lon_max;
  uint32_t npts, nlines, ncities;
  SDMapPt* pts;
  SDMapLine* lines;
  SDMapCity* cities;
};

class DeckHW;

class SDMaps {
public:
  // scans /meshdeck-maps/ on the SD card; returns number of packs loaded
  int load(DeckHW& hw);
  int count() const { return _n; }
  const SDMapPack* pack(int i) const { return (i >= 0 && i < _n) ? &_packs[i] : nullptr; }

private:
  bool loadFile(const char* path);
  SDMapPack _packs[SDMAP_MAX_PACKS];
  int _n = 0;
};
