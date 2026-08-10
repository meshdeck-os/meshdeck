# NewMaps (OSM vector basemap)

NewMaps is a second map app on the home screen. The classic **Map** app is unchanged.

## What you need

1. Firmware with NewMaps support  
2. An SD card with one or more **`.mdv`** packs in `/meshdeck-maps/`  
3. Optional: classic **`.mdm`** packs can sit in the same folder (used only by old Map)

## Build a Northern Idaho pack

### Option A — Overpass (no local OSM file)

```bash
python tools/gen_newmap.py --bbox -117.5 46.0 -115.5 49.0 ^
  -o sdcard/meshdeck-maps/northern-idaho.mdv
```

Smaller / faster (Coeur d’Alene metro):

```bash
python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 ^
  -o sdcard/meshdeck-maps/cda.mdv --buildings
```

### Option B — Geofabrik PBF (recommended for quality)

1. Download: https://download.geofabrik.de/north-america/us/idaho-latest.osm.pbf  
2. Install: `pip install osmium`  
3. Run:

```bash
python tools/gen_newmap.py --pbf idaho-latest.osm.pbf ^
  --bbox -117.5 46.0 -115.5 49.0 ^
  -o sdcard/meshdeck-maps/northern-idaho.mdv
```

### Install on device

Copy the `.mdv` file to the SD card:

```
/meshdeck-maps/northern-idaho.mdv
```

Boot MeshDeck → home → **NewMaps**.

## Controls

| Input | Action |
|-------|--------|
| `+` / `=` / `]` | Zoom in |
| `-` / `_` / `[` | Zoom out |
| Trackball | Pan |
| `c` | Center on me (or pack center) |
| `i` | Jump to Coeur d’Alene area |
| Tap left / right | Zoom out / in |
| Drag | Pan |

## Design notes

- **Source data:** OpenStreetMap (vector), processed offline into `.mdv`  
- **Zoom filtering:** each feature has `min_scale`; minor roads/paths/buildings appear only when zoomed in  
- **Smooth roads:** densified polylines + multi-stroke casings on the device  
- **Palette:** light “paper” basemap (water, parks, road hierarchy)  
- **Attribution:** © OpenStreetMap contributors (ODbL)

## Format

See `firmware/ui/NewMap.h` for the binary layout (`MDV1`).
