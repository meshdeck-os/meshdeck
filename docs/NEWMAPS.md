# NewMaps (OSM vector basemap)

NewMaps is a second map app on the home screen. The classic **Map** app is unchanged
(built-in coastlines plus optional `.mdm` packs). NewMaps is the street-level
offline basemap: roads, water, parks, rail, places, live mesh nodes.

This page is the full guide: generate a pack, copy it to the SD card, and use
the app. For the classic coastline packs see **FLASHING.md** (`tools/gen_sdmap.py`).

## What you need

1. Firmware with NewMaps (splash / About shows `1.0.0_260814h` or later)
2. A FAT32 SD card
3. One or more **MDV2 v3** `.mdv` packs in `/meshdeck-maps/`
4. Optional: classic **`.mdm`** packs can sit in the same folder (used only by old Map)

Old **MDV1** files still load, but they keep the whole region in RAM and show
fragmented roads. Firmware skips MDV1 when any MDV2 file is present, so do not
leave a leftover giant MDV1 next to the new `_00.mdv` files.

MDV2 **v2** files still draw. Long-press will only show the road class, not a
name -- rebuild with the current `tools/gen_newmap.py` for names.

## Why roads looked broken

OSM stores each street as many short ways. The old pack dumped every way into
one file, hit a point budget mid-stream (so random later roads vanished), and
dropped vertices that crossed the bbox instead of clipping them. The T-Deck
then loaded that whole blob into PSRAM and scanned it every frame.

## What MDV2 does (T-Deck / ESP32)

One area can be **several `.mdv` region files**. `gen_newmap.py` splits a bbox
into smaller files when a cell would exceed `--max-file-pts` (default 120k verts).
Cells overlap by `--overlap` degrees so roads that cross a boundary exist in
both files. The device draws every region that intersects the view (up to 16
files), same paint order (all casings, then all fills), so edges meet.

The `-o` path is a *stem*. A small bbox writes `northern-idaho.mdv`. A large
bbox writes `northern-idaho_00.mdv`, `_01.mdv`, ... Copy **every** `_NN.mdv`
file -- a single leftover file is only one cell of the region.

| LOD | Contents | Firmware shows at | Where |
|-----|----------|-------------------|--------|
| 0 overview | Stitched motorway / trunk / primary / secondary / tertiary + water + cities. Signed I / US / state routes always live here. | Always (majors stay on at 15-30 km views) | PSRAM |
| 1 tiles | Residential, landuse, rail (~0.75 deg cells) | Scale >= 384 | 12-slot cache |
| 2 tiles | Service, path, buildings (~0.25 deg cells) | Scale >= 1024 | 12-slot cache |

The device draws the overview immediately (continuous major roads), then pages
only the tiles that cover the current view. At most 4 tiles are read per frame.
SD is opened only for those reads, then released -- it shares SPI with the display.

During a finger-drag the last full frame is slid in PSRAM (no vector rebuild,
no SD). Lift to redraw. That is what keeps swipe-pan fast.

Status `NewMaps  zN ...` means detail tiles are still loading. `c OSM` is the
OpenStreetMap attribution.

## Generate a pack

Python 3.8+ is enough. For a local PBF (recommended) also install osmium:

```
pip install osmium
```

`--bbox` is always required, west / south / east / north in decimal degrees:

```
--bbox LON_MIN LAT_MIN LON_MAX LAT_MAX
```

Pick a box you actually walk around, not a whole state. A 2 x 3 degree box
around a metro is typical. Bigger boxes become more `_NN.mdv` files (cap 16)
and a heavier overview.

### Option A -- Geofabrik PBF (recommended)

No Overpass quota, repeatable, and it walks `route=road` relations so numbered
highways stay one line through town.

1. Download a regional extract from https://download.geofabrik.de/
   (US example: https://download.geofabrik.de/north-america/us/idaho-latest.osm.pbf)
2. Install osmium: `pip install osmium`
3. Run (bash / macOS / Linux):

```bash
python tools/gen_newmap.py --pbf idaho-latest.osm.pbf \
  --bbox -117.5 46.0 -115.5 49.0 \
  -o sdcard/meshdeck-maps/northern-idaho.mdv
```

Windows cmd.exe (same args, `^` continues the line):

```bat
python tools\gen_newmap.py --pbf idaho-latest.osm.pbf ^
  --bbox -117.5 46.0 -115.5 49.0 ^
  -o sdcard\meshdeck-maps\northern-idaho.mdv
```

PowerShell can use the bash form if you put the command on one line.

A Coeur d'Alene metro box with building footprints (slower, LOD2 heavier):

```bash
python tools/gen_newmap.py --pbf idaho-latest.osm.pbf \
  --bbox -117.0 47.5 -116.4 47.9 \
  -o sdcard/meshdeck-maps/cda.mdv --buildings
```

The generator walks OSM `route=road` relations (`network=US:I`, `US:US`,
`US:ID`, ...) plus every way with a `ref=`. Those signed routes (interstates,
US highways, state routes) are stitched into continuous overview lines and
always stay in RAM. OSM often retags a state highway as residential through
town -- the route relation is what keeps it one line. Unsigned local collectors
do not eat the overview budget.

Rebuild the pack after any `gen_newmap.py` or MDV version change.

### Option B -- Overpass (no local OSM file)

Omit `--pbf` / `--geojson` and the script queries Overpass for the bbox.
Fine for a small city. Timeouts and rate limits are common on a county-sized
box -- fall back to Option A.

```bash
python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 \
  -o sdcard/meshdeck-maps/cda.mdv
```

It retries three times. `--overpass` forces this path if you also have a PBF
sitting around and want to ignore it.

### Option C -- GeoJSON

One or more FeatureCollections (`--geojson` may be repeated). Ways need the
same OSM-style properties the PBF path uses (`highway`, `ref`, `name`,
`waterway`, `natural`, `landuse`, `place`, ...).

```bash
python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 \
  --geojson roads.geojson --geojson water.geojson \
  -o sdcard/meshdeck-maps/cda.mdv
```

### CLI reference

| Flag | Default | Meaning |
|------|---------|---------|
| `--bbox W S E N` | required | West / south / east / north in degrees |
| `-o` / `--output` | required | Output `.mdv` path (stem; may become `_00.mdv` ...) |
| `--pbf FILE` | | Local `.osm.pbf` (needs `pip install osmium`) |
| `--geojson FILE` | | Repeatable GeoJSON FeatureCollection |
| `--overpass` | auto if no PBF/GeoJSON | Force Overpass fetch |
| `--buildings` | off | Include building footprints (LOD2 only) |
| `--scale` | auto (~1e5) | Stored coord scale |
| `--simplify` | 0.00008 | Douglas-Peucker tolerance in degrees (LOD1; LOD0 is coarser) |
| `--tile-lod1` | 0.75 | LOD1 tile size in degrees |
| `--tile-lod2` | 0.25 | LOD2 tile size in degrees |
| `--max-overview-pts` | 100000 | Vertices kept in the always-RAM overview |
| `--max-tile-pts` | 800000 | Vertex budget across one tiled LOD |
| `--max-file-pts` | 120000 | Split into more `.mdv` files past this many verts |
| `--overlap` | 0.03 | Degrees of overlap between region files |

A typical run prints how many region files it wrote and the LOD point counts.
If it says `dropped N over budget`, shrink the bbox or accept missing minor
streets at the edge of the budget.

### What is included

| Layer | Source (OSM tags) | When it draws |
|-------|-------------------|---------------|
| Motorway / trunk / primary / secondary / tertiary | `highway=*`, plus signed `ref` / `route=road` | Overview (always) |
| Signed I / US / state routes | `network=US:I` / `US:US` / `US:ID` / way `ref=` | Forced onto overview even if tagged residential |
| Residential / unclassified | `highway=residential` etc. | Zoomed in (LOD1) |
| Service, driveway, path | `highway=service` / `path` / `footway` ... | Close zoom (LOD2) |
| Rail | `railway=rail` | LOD1 |
| Water areas | lake / reservoir / pond multipolygons | Overview |
| Rivers / canals / streams | `waterway=*` | By class |
| Parks / forest | `leisure=park`, `landuse=forest`, ... | Parks early; forest with landuse |
| Cities / towns / villages | `place=*` | Labels, earlier for larger places |
| Buildings | `building=*` | Only with `--buildings` |

Farmland / meadow / grass polygons are omitted on purpose -- they ate the
per-tile point budget and streets were dropped.

Road fragments of the *same* named street (or same signed `ref`) are merged.
Different streets that only meet at a junction stay separate so long-press
cannot pick the connecting road.

### Install on the device

1. Format the card FAT32 if needed.
2. Create `/meshdeck-maps/` in the card root (same folder as classic `.mdm` packs).
3. Copy **every** generated `.mdv` (`northern-idaho.mdv` or the whole
   `northern-idaho_00.mdv` ... `_12.mdv` set).
4. Insert the card, boot MeshDeck, open **NewMaps** from the home grid.
   Packs also load at boot; **Settings -> Reload SD map packs** if you swap
   the card while powered.

The terminal logs each file (`newmap: northern-idaho_00.mdv v2 ...`). An MDV1
file also prints a rebuild hint.

Do not commit Geofabrik `.osm.pbf` extracts or generated `_NN.mdv` binaries to
git -- they are large and region-specific. Keep the generator + this doc.

## Controls

| Input | Action |
|-------|--------|
| `+` / `=` / `]` / `o` / `q` | Zoom in (`+` is printed on **O** on the T-Deck) |
| `-` / `_` / `[` / `i` / `a` | Zoom out (`-` is printed on **I**) |
| Symbol layer then `o` / `i` | Also zoom in / out |
| Trackball | Pan |
| Trackball click | Zoom in one step |
| `c` | Center on me (or pack center) |
| Esc / Backspace / Back | Close the info card, or leave the app |
| Swipe / drag | Pan (slides last frame, full redraw on lift) |
| Double-tap | Zoom in |
| Long-press | Feature info (name / ref / class / place of the road under your finger) |
| Pinch open / close | Zoom in / out |
| Single tap | Does nothing (no more left/right zoom) |

Long-press hits the drawn road, then reads that feature's `name_id` (MDV2 v3).
No nearest-label guess. Unnamed roads show the class only (`Residential`,
`State route`, ...). Tap again or press Back to dismiss.

## Design notes

- **Source data:** OpenStreetMap (vector), processed offline into `.mdv`
- **Stitched roads:** same name or same signed `ref` only
- **Zoom floors (firmware):** majors always; residential ~384; service ~1024; driveway / path / buildings ~1536-2048. Streets stay hidden at region zoom.
- **Widths:** hairline when zoomed out; driveway < service < residential < tertiary < secondary < primary < trunk < motorway
- **Junctions:** all casings first, then all fills (disk-capsule strokes)
- **Water:** lakes / reservoirs / ponds are filled blue (OSM multipolygons assembled). Rivers / canals / streams are stroked by class.
- **Palette:** light paper basemap
- **Attribution:** (c) OpenStreetMap contributors (ODbL)

## Format

See `firmware/ui/NewMap.h` for MDV1 and MDV2 layouts (`MDV2` = tiled LOD).

MDV2 **v3** (current `gen_newmap.py`) stores a `u16 name_id` on every feature
and a `NewMapName` table (`name`, `ref`, `place`). Rebuild packs after this
change.

## Troubleshooting

| Symptom | What to try |
|---------|-------------|
| Empty NewMaps / "no NewMaps .mdv packs" | Card is FAT32? Folder is exactly `/meshdeck-maps/` (not nested)? Files end in `.mdv`? |
| Only a slice of the region | Copy every `_NN.mdv`, not just `_00` |
| Broken / missing numbered highways | Rebuild with current `gen_newmap.py` from a PBF so route relations are walked |
| Long-press shows class but no name | Pack is MDV2 v2; rebuild for v3 |
| Streets vanish at town zoom | Overview budget ate unsigned collectors; expected. Zoom in for LOD1. Or shrink the bbox. |
| Pinch does nothing | Need firmware `1.0.0_260814h`+ (GT911 reads each finger in its own I2C transaction) |
| Overpass timeouts | Use a Geofabrik PBF |
| Generator `dropped N over budget` | Smaller `--bbox`, or raise `--max-file-pts` / `--max-tile-pts` carefully |

## Classic Map packs (`.mdm`)

The other home-screen **Map** app does not read `.mdv`. It uses the built-in
worldwide coastline (from `tools/gen_mapdata.py`) plus optional Natural Earth
`.mdm` files in the same `/meshdeck-maps/` folder.

```
python tools/gen_sdmap.py <natural-earth-geojson> uk.mdm -11 49.5 2.2 61.2
```

Data: https://github.com/martynafford/natural-earth-geojson
(needs `10m/physical/ne_10m_coastline.json` and
`50m/cultural/ne_50m_populated_places_simple.json`).

A ready-made set lives in `sdcard/meshdeck-maps/` (`uk.mdm`, `use.mdm`, ...).
See **FLASHING.md**.
