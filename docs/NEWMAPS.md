# NewMaps (OSM vector basemap)

NewMaps is a second map app on the home screen. The classic **Map** app is unchanged.

## What you need

1. Firmware with NewMaps support (splash / About shows `1.0.0_260813e` or later)
2. An SD card with one or more **`.mdv`** packs in `/meshdeck-maps/`
3. Optional: classic **`.mdm`** packs can sit in the same folder (used only by old Map)

Use **MDV2** packs (current `tools/gen_newmap.py`). Old MDV1 files still load, but they keep the whole region in RAM and show fragmented roads.

## Why roads looked broken

OSM stores each street as many short ways. The old pack dumped every way into one file, hit a point budget mid-stream (so random later roads vanished), and dropped vertices that crossed the bbox instead of clipping them. The T-Deck then loaded that whole blob into PSRAM and scanned it every frame.

## What MDV2 does (T-Deck / ESP32)

One `.mdv` per region -- **not** a folder of tile files. FAT32 + the shared SPI SD bus is a poor fit for hundreds of tiny files.

| LOD | Contents | Firmware shows at | Where |
|-----|----------|-------------------|--------|
| 0 overview | Stitched motorway / trunk / primary / secondary / tertiary + water + cities | Always (majors stay on at 15-30 km views) | PSRAM |
| 1 tiles | Residential, landuse, rail, parks (~0.75 deg cells) | Scale >= 512 | 9-slot cache |
| 2 tiles | Service, path, buildings (~0.25 deg cells) | Scale >= 1024 | 9-slot cache |

The device draws the overview immediately (continuous major roads), then pages only the tiles that cover the current view. At most 2 tiles are read per frame. SD is opened only for those reads, then released -- it shares SPI with the display.

During a finger-drag the last full frame is slid in PSRAM (no vector rebuild, no SD). Lift to redraw. That is what keeps swipe-pan fast.

## Build a Northern Idaho pack

### Option A -- Overpass (no local OSM file)

```bash
python tools/gen_newmap.py --bbox -117.5 46.0 -115.5 49.0 ^
  -o sdcard/meshdeck-maps/northern-idaho.mdv
```

Smaller / faster (Coeur d'Alene metro):

```bash
python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 ^
  -o sdcard/meshdeck-maps/cda.mdv --buildings
```

### Option B -- Geofabrik PBF (recommended)

1. Download: https://download.geofabrik.de/north-america/us/idaho-latest.osm.pbf
2. Install: `pip install osmium`
3. Run:

```bash
python tools/gen_newmap.py --pbf idaho-latest.osm.pbf ^
  --bbox -117.5 46.0 -115.5 49.0 ^
  -o sdcard/meshdeck-maps/northern-idaho.mdv
```

Rebuild the pack after this MDV2 work. A new pack stitches numbered routes (`ref=US 95`, `I 90`, `ID 3`) even when OSM tags a downtown stretch as residential, so state routes stay continuous through cities.

### Install on device

Copy the `.mdv` file to the SD card:

```
/meshdeck-maps/northern-idaho.mdv
```

Boot MeshDeck -> home -> **NewMaps**. Status `zN ...` means detail tiles are still loading.

## Controls

| Input | Action |
|-------|--------|
| `+` / `=` / `]` / `o` / `q` | Zoom in (`+` is printed on **O** on the T-Deck) |
| `-` / `_` / `[` / `i` / `a` | Zoom out (`-` is printed on **I**) |
| Symbol layer then `o` / `i` | Also zoom in / out |
| Trackball | Pan |
| Trackball click | Zoom in one step |
| `c` | Center on me (or pack center) |
| Swipe / drag | Pan (slides last frame, full redraw on lift) |
| Double-tap | Zoom in |
| Pinch open / close | Zoom in / out |
| Single tap | Does nothing (no more left/right zoom) |

## Design notes

- **Source data:** OpenStreetMap (vector), processed offline into `.mdv`
- **Stitched roads:** same-class OSM ways that share endpoints are merged; numbered `ref=` ways are promoted onto the regional network
- **Zoom floors (firmware):** majors always; residential ~512; service ~1024; driveway / path / buildings ~1536-2048. Streets stay hidden at region zoom.
- **Widths:** hairline when zoomed out; driveway < service < residential < tertiary < secondary < primary < trunk < motorway
- **Junctions:** all casings first, then all fills (disk-capsule strokes)
- **Rivers:** width by class (river / canal / stream)
- **Palette:** light paper basemap
- **Attribution:** (c) OpenStreetMap contributors (ODbL)

## Format

See `firmware/ui/NewMap.h` for MDV1 and MDV2 layouts (`MDV2` = tiled LOD).
