#!/usr/bin/env python3
"""
Generate MeshDeck NewMaps packs (.mdv) from OpenStreetMap.

The .mdv format is a multi-layer vector basemap optimized for the T-Deck:
  roads (by class), water, landuse, optional buildings, place labels.
Features carry min_scale so the device filters by zoom.

Sources (pick one):
  A) Overpass API  — no local OSM install (default)
  B) Local GeoJSON FeatureCollection(s)
  C) Local .osm / .osm.pbf via optional `osmium` Python module

Examples — Northern Idaho (Coeur d'Alene / Sandpoint / Palouse fringe):

  # Fetch from Overpass (needs network)
  python tools/gen_newmap.py --bbox -117.5 46.0 -115.5 49.0 \\
      -o sdcard/meshdeck-maps/northern-idaho.mdv

  # Smaller, faster pack (CDA area only)
  python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 \\
      -o sdcard/meshdeck-maps/cda.mdv --buildings

  # From Geofabrik extract (requires: pip install osmium)
  python tools/gen_newmap.py --pbf idaho-latest.osm.pbf \\
      --bbox -117.5 46.0 -115.5 49.0 -o northern-idaho.mdv

Copy the .mdv onto the SD card under /meshdeck-maps/ next to any classic .mdm packs.

License: OpenStreetMap data is © OpenStreetMap contributors (ODbL).
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import time
import urllib.parse
import urllib.request
from collections import defaultdict

MAGIC = 0x3156444D  # "MDV1"
VERSION = 1

# Layer IDs must match firmware/ui/NewMap.h
NML_WATER_AREA = 0
NML_LANDUSE_PARK = 1
NML_LANDUSE_FOREST = 2
NML_LANDUSE_RESIDENTIAL = 3
NML_LANDUSE_INDUSTRIAL = 4
NML_WATER_LINE = 5
NML_RAIL = 6
NML_ROAD_PATH = 7
NML_ROAD_SERVICE = 8
NML_ROAD_RESIDENTIAL = 9
NML_ROAD_TERTIARY = 10
NML_ROAD_SECONDARY = 11
NML_ROAD_PRIMARY = 12
NML_ROAD_TRUNK = 13
NML_ROAD_MOTORWAY = 14
NML_BUILDING = 15

NMF_CLOSED = 0x01

# highway=* → (layer, min_scale px/deg)
HIGHWAY_MAP = {
    "motorway": (NML_ROAD_MOTORWAY, 16),
    "motorway_link": (NML_ROAD_MOTORWAY, 48),
    "trunk": (NML_ROAD_TRUNK, 16),
    "trunk_link": (NML_ROAD_TRUNK, 48),
    "primary": (NML_ROAD_PRIMARY, 24),
    "primary_link": (NML_ROAD_PRIMARY, 64),
    "secondary": (NML_ROAD_SECONDARY, 32),
    "secondary_link": (NML_ROAD_SECONDARY, 96),
    "tertiary": (NML_ROAD_TERTIARY, 48),
    "tertiary_link": (NML_ROAD_TERTIARY, 128),
    "unclassified": (NML_ROAD_RESIDENTIAL, 64),
    "residential": (NML_ROAD_RESIDENTIAL, 96),
    "living_street": (NML_ROAD_RESIDENTIAL, 128),
    "service": (NML_ROAD_SERVICE, 192),
    "track": (NML_ROAD_PATH, 192),
    "path": (NML_ROAD_PATH, 256),
    "footway": (NML_ROAD_PATH, 384),
    "cycleway": (NML_ROAD_PATH, 256),
    "bridleway": (NML_ROAD_PATH, 384),
    "steps": (NML_ROAD_PATH, 512),
    "pedestrian": (NML_ROAD_PATH, 256),
}

WATERWAY_LINE = {
    "river": 24,
    "canal": 48,
    "stream": 128,
    "drain": 256,
    "ditch": 384,
}

LANDUSE_MAP = {
    "residential": NML_LANDUSE_RESIDENTIAL,
    "industrial": NML_LANDUSE_INDUSTRIAL,
    "commercial": NML_LANDUSE_RESIDENTIAL,
    "retail": NML_LANDUSE_RESIDENTIAL,
    "forest": NML_LANDUSE_FOREST,
    "wood": NML_LANDUSE_FOREST,
    "farmland": NML_LANDUSE_PARK,
    "meadow": NML_LANDUSE_PARK,
    "grass": NML_LANDUSE_PARK,
    "orchard": NML_LANDUSE_PARK,
    "vineyard": NML_LANDUSE_PARK,
    "recreation_ground": NML_LANDUSE_PARK,
    "village_green": NML_LANDUSE_PARK,
    "allotments": NML_LANDUSE_PARK,
    "cemetery": NML_LANDUSE_PARK,
}

LEISURE_PARK = {"park", "nature_reserve", "garden", "pitch", "golf_course", "playground"}
NATURAL_WATER = {"water", "bay", "strait"}
NATURAL_WOOD = {"wood", "scrub"}

PLACE_KIND = {
    "city": (0, 16),
    "town": (1, 32),
    "village": (2, 64),
    "hamlet": (3, 128),
    "suburb": (4, 96),
    "neighbourhood": (4, 192),
    "locality": (3, 256),
}


def douglas_peucker(coords, tol):
    """coords: list of (lon, lat). tol in degrees."""
    if len(coords) <= 2 or tol <= 0:
        return coords

    def dist_point_seg(p, a, b):
        # perpendicular distance in lon/lat degrees (approx)
        ax, ay = a
        bx, by = b
        px, py = p
        dx, dy = bx - ax, by - ay
        if dx == 0 and dy == 0:
            return math.hypot(px - ax, py - ay)
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - (ax + t * dx), py - (ay + t * dy))

    def rec(pts):
        if len(pts) <= 2:
            return pts
        a, b = pts[0], pts[-1]
        max_d, idx = 0.0, 0
        for i in range(1, len(pts) - 1):
            d = dist_point_seg(pts[i], a, b)
            if d > max_d:
                max_d, idx = d, i
        if max_d > tol:
            left = rec(pts[: idx + 1])
            right = rec(pts[idx:])
            return left[:-1] + right
        return [a, b]

    return rec(coords)


def densify(coords, max_step_deg):
    """Insert vertices so long edges don't look jagged when projected."""
    if len(coords) < 2 or max_step_deg <= 0:
        return coords
    out = [coords[0]]
    for i in range(1, len(coords)):
        lon0, lat0 = out[-1]
        lon1, lat1 = coords[i]
        d = math.hypot(lon1 - lon0, lat1 - lat0)
        n = int(d / max_step_deg)
        if n > 1:
            if n > 24:
                n = 24
            for k in range(1, n):
                t = k / n
                out.append((lon0 + (lon1 - lon0) * t, lat0 + (lat1 - lat0) * t))
        out.append((lon1, lat1))
    return out


def clip_ring(coords, bbox):
    """Keep points inside bbox with a soft margin; drop tiny fragments."""
    lon0, lat0, lon1, lat1 = bbox
    m = 0.02
    out = []
    for lon, lat in coords:
        if lon0 - m <= lon <= lon1 + m and lat0 - m <= lat <= lat1 + m:
            out.append((lon, lat))
    return out


class Builder:
    def __init__(self, bbox, scale, simplify_tol, densify_step, max_pts=600000):
        self.bbox = bbox  # lon0,lat0,lon1,lat1
        self.scale = scale
        self.tol = simplify_tol
        self.densify_step = densify_step
        self.max_pts = max_pts
        self.pts = []  # (lat_s, lon_s)
        self.feats = []  # (layer, flags, min_scale, start, count)
        self.labels = []  # (lat, lon, kind, min_scale, name)

    def add_line(self, coords, layer, min_scale, closed=False):
        if len(self.pts) >= self.max_pts:
            return
        coords = clip_ring(coords, self.bbox)
        if len(coords) < 2:
            return
        coords = douglas_peucker(coords, self.tol)
        if layer >= NML_ROAD_RESIDENTIAL or layer in (
            NML_ROAD_PATH,
            NML_ROAD_SERVICE,
            NML_WATER_LINE,
        ):
            coords = densify(coords, self.densify_step)
        if len(coords) < 2:
            return
        if closed and len(coords) >= 3:
            if coords[0] != coords[-1]:
                coords = coords + [coords[0]]
        start = len(self.pts)
        for lon, lat in coords:
            if len(self.pts) >= self.max_pts:
                break
            self.pts.append(
                (int(round(lat * self.scale)), int(round(lon * self.scale)))
            )
        count = len(self.pts) - start
        if count < 2:
            if count:
                self.pts.pop()
            return
        flags = NMF_CLOSED if closed else 0
        self.feats.append((layer, flags, int(min_scale), start, count))

    def add_label(self, lon, lat, kind, min_scale, name):
        lon0, lat0, lon1, lat1 = self.bbox
        if not (lon0 <= lon <= lon1 and lat0 <= lat <= lat1):
            return
        name = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in name)[:19]
        if not name:
            return
        self.labels.append((lat, lon, kind, min_scale, name))

    def write(self, path):
        lon0, lat0, lon1, lat1 = self.bbox
        with open(path, "wb") as out:
            out.write(
                struct.pack(
                    "<IHH4f f III",
                    MAGIC,
                    VERSION,
                    0,  # flags
                    float(lat0),
                    float(lat1),
                    float(lon0),
                    float(lon1),
                    float(self.scale),
                    len(self.pts),
                    len(self.feats),
                    len(self.labels),
                )
            )
            for lat_s, lon_s in self.pts:
                out.write(struct.pack("<ii", lat_s, lon_s))
            for layer, flags, min_scale, start, count in self.feats:
                out.write(
                    struct.pack(
                        "<BBHII",
                        layer & 0xFF,
                        flags & 0xFF,
                        min(min_scale, 65535),
                        start,
                        count,
                    )
                )
            for lat, lon, kind, min_scale, name in self.labels:
                msd = min(255, max(0, int(min_scale) // 4))
                nb = name.encode("ascii", "replace")[:19]
                nb = nb + b"\0" * (20 - len(nb))
                out.write(
                    struct.pack(
                        "<iiBB20s",
                        int(round(lat * self.scale)),
                        int(round(lon * self.scale)),
                        kind & 0xFF,
                        msd,
                        nb,
                    )
                )
        kb = (
            32
            + len(self.pts) * 8
            + len(self.feats) * 12
            + len(self.labels) * 30
        ) / 1024
        print(
            f"Wrote {path}: {len(self.pts)} pts, {len(self.feats)} feats, "
            f"{len(self.labels)} labels, {kb:.0f} KB, scale={self.scale}"
        )


def overpass_query(bbox, buildings=False, timeout=180):
    lon0, lat0, lon1, lat1 = bbox
    # Overpass uses (south,west,north,east)
    bb = f"{lat0},{lon0},{lat1},{lon1}"
    parts = [
        f'way["highway"]({bb});',
        f'relation["highway"]({bb});',
        f'way["waterway"]({bb});',
        f'way["natural"="water"]({bb});',
        f'relation["natural"="water"]({bb});',
        f'way["water"="lake"]({bb});',
        f'relation["water"]({bb});',
        f'way["landuse"]({bb});',
        f'relation["landuse"]({bb});',
        f'way["leisure"]({bb});',
        f'way["natural"="wood"]({bb});',
        f'way["railway"~"rail|light_rail|subway"]({bb});',
        f'node["place"~"city|town|village|hamlet|suburb|neighbourhood|locality"]({bb});',
    ]
    if buildings:
        parts.append(f'way["building"]({bb});')
    q = f"""
[out:json][timeout:{timeout}];
(
  {"".join(parts)}
);
out body;
>;
out skel qt;
"""
    url = "https://overpass-api.de/api/interpreter"
    data = urllib.parse.urlencode({"data": q}).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"User-Agent": "MeshDeck-gen_newmap/1.0"}
    )
    print("Querying Overpass API (may take a minute)...")
    with urllib.request.urlopen(req, timeout=timeout + 30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def build_from_overpass(b: Builder, osm_json, buildings=False):
    nodes = {}
    ways = {}
    relations = []
    for el in osm_json.get("elements", []):
        t = el.get("type")
        if t == "node":
            nodes[el["id"]] = (el["lon"], el["lat"])
            tags = el.get("tags") or {}
            place = tags.get("place")
            if place in PLACE_KIND:
                kind, ms = PLACE_KIND[place]
                name = tags.get("name") or tags.get("name:en") or ""
                if name:
                    b.add_label(el["lon"], el["lat"], kind, ms, name)
        elif t == "way":
            ways[el["id"]] = el
        elif t == "relation":
            relations.append(el)

    def way_coords(w):
        coords = []
        for nid in w.get("nodes") or []:
            if nid in nodes:
                coords.append(nodes[nid])
        return coords

    for wid, w in ways.items():
        tags = w.get("tags") or {}
        coords = way_coords(w)
        if len(coords) < 2:
            continue

        if "highway" in tags:
            hw = tags["highway"]
            if hw in HIGHWAY_MAP:
                layer, ms = HIGHWAY_MAP[hw]
                b.add_line(coords, layer, ms, closed=False)
            continue

        if tags.get("railway") in ("rail", "light_rail", "subway", "tram"):
            b.add_line(coords, NML_RAIL, 64, closed=False)
            continue

        if "waterway" in tags:
            ww = tags["waterway"]
            ms = WATERWAY_LINE.get(ww, 128)
            b.add_line(coords, NML_WATER_LINE, ms, closed=False)
            continue

        if tags.get("natural") in NATURAL_WATER or tags.get("water"):
            b.add_line(coords, NML_WATER_AREA, 24, closed=True)
            continue

        if tags.get("natural") in NATURAL_WOOD or tags.get("landuse") == "forest":
            b.add_line(coords, NML_LANDUSE_FOREST, 48, closed=True)
            continue

        if tags.get("leisure") in LEISURE_PARK:
            b.add_line(coords, NML_LANDUSE_PARK, 48, closed=True)
            continue

        lu = tags.get("landuse")
        if lu in LANDUSE_MAP:
            layer = LANDUSE_MAP[lu]
            ms = 32 if layer != NML_LANDUSE_RESIDENTIAL else 64
            b.add_line(coords, layer, ms, closed=True)
            continue

        if buildings and "building" in tags:
            b.add_line(coords, NML_BUILDING, 384, closed=True)

    # Multipolygon water/landuse from relations (outer ways only, simplified)
    for rel in relations:
        tags = rel.get("tags") or {}
        layer = None
        ms = 32
        if (
            tags.get("natural") in NATURAL_WATER
            or tags.get("water")
            or (
                tags.get("type") == "multipolygon"
                and tags.get("natural") == "water"
            )
        ):
            layer, ms = NML_WATER_AREA, 24
        elif tags.get("landuse") in LANDUSE_MAP:
            layer = LANDUSE_MAP[tags["landuse"]]
            ms = 48
        elif tags.get("leisure") in LEISURE_PARK:
            layer, ms = NML_LANDUSE_PARK, 48
        if layer is None:
            continue
        for mem in rel.get("members") or []:
            if mem.get("type") != "way" or mem.get("role") not in ("outer", ""):
                continue
            w = ways.get(mem.get("ref"))
            if not w:
                continue
            coords = way_coords(w)
            if len(coords) >= 3:
                b.add_line(coords, layer, ms, closed=True)


def build_from_geojson(b: Builder, path, default_layer=NML_ROAD_RESIDENTIAL, min_scale=64):
    with open(path, encoding="utf-8") as f:
        gj = json.load(f)
    feats = gj["features"] if gj.get("type") == "FeatureCollection" else [gj]
    for feat in feats:
        props = feat.get("properties") or {}
        g = feat.get("geometry") or {}
        gtype = g.get("type")
        coords = g.get("coordinates")
        layer = default_layer
        ms = min_scale
        closed = False
        if "highway" in props and props["highway"] in HIGHWAY_MAP:
            layer, ms = HIGHWAY_MAP[props["highway"]]
        if gtype == "LineString":
            b.add_line(coords, layer, ms, closed=False)
        elif gtype == "MultiLineString":
            for line in coords:
                b.add_line(line, layer, ms, closed=False)
        elif gtype == "Polygon":
            if coords:
                b.add_line(coords[0], layer, ms, closed=True)
        elif gtype == "MultiPolygon":
            for poly in coords:
                if poly:
                    b.add_line(poly[0], layer, ms, closed=True)
        elif gtype == "Point":
            name = props.get("name") or props.get("name:en") or ""
            place = props.get("place", "village")
            kind, pms = PLACE_KIND.get(place, (2, 64))
            if name and len(coords) >= 2:
                b.add_label(coords[0], coords[1], kind, pms, name)


def build_from_pbf(b: Builder, pbf_path, bbox, buildings=False):
    try:
        import osmium
    except ImportError:
        print("osmium not installed. pip install osmium", file=sys.stderr)
        sys.exit(2)

    lon0, lat0, lon1, lat1 = bbox

    class Handler(osmium.SimpleHandler):
        def __init__(self):
            super().__init__()
            self.ways_done = 0

        def node(self, n):
            if not n.location.valid():
                return
            lon, lat = n.location.lon, n.location.lat
            if not (lon0 <= lon <= lon1 and lat0 <= lat <= lat1):
                return
            place = n.tags.get("place")
            if place in PLACE_KIND:
                kind, ms = PLACE_KIND[place]
                name = n.tags.get("name") or ""
                if name:
                    b.add_label(lon, lat, kind, ms, name)

        def way(self, w):
            if not w.is_closed() and "highway" not in w.tags and "waterway" not in w.tags:
                if "building" not in w.tags and "landuse" not in w.tags:
                    if "natural" not in w.tags and "leisure" not in w.tags:
                        if "railway" not in w.tags:
                            return
            coords = []
            try:
                for n in w.nodes:
                    if n.location.valid():
                        coords.append((n.location.lon, n.location.lat))
            except osmium.InvalidLocationError:
                return
            if len(coords) < 2:
                return
            tags = {k: v for k, v in w.tags}
            if "highway" in tags and tags["highway"] in HIGHWAY_MAP:
                layer, ms = HIGHWAY_MAP[tags["highway"]]
                b.add_line(coords, layer, ms, closed=False)
            elif tags.get("railway") in ("rail", "light_rail", "subway", "tram"):
                b.add_line(coords, NML_RAIL, 64, closed=False)
            elif "waterway" in tags:
                ms = WATERWAY_LINE.get(tags["waterway"], 128)
                b.add_line(coords, NML_WATER_LINE, ms, closed=False)
            elif tags.get("natural") in NATURAL_WATER or "water" in tags:
                b.add_line(coords, NML_WATER_AREA, 24, closed=True)
            elif tags.get("natural") in NATURAL_WOOD or tags.get("landuse") == "forest":
                b.add_line(coords, NML_LANDUSE_FOREST, 48, closed=True)
            elif tags.get("leisure") in LEISURE_PARK:
                b.add_line(coords, NML_LANDUSE_PARK, 48, closed=True)
            elif tags.get("landuse") in LANDUSE_MAP:
                layer = LANDUSE_MAP[tags["landuse"]]
                b.add_line(coords, layer, 48, closed=True)
            elif buildings and "building" in tags:
                b.add_line(coords, NML_BUILDING, 384, closed=True)
            self.ways_done += 1
            if self.ways_done % 5000 == 0:
                print(f"  … {self.ways_done} ways, {len(b.pts)} pts")

    print(f"Reading {pbf_path} (locations=True)...")
    h = Handler()
    h.apply_file(pbf_path, locations=True)


def main():
    ap = argparse.ArgumentParser(description="Build MeshDeck NewMaps .mdv packs from OSM")
    ap.add_argument(
        "--bbox",
        nargs=4,
        type=float,
        metavar=("LON_MIN", "LAT_MIN", "LON_MAX", "LAT_MAX"),
        required=True,
        help="Bounding box (Northern Idaho e.g. -117.5 46.0 -115.5 49.0)",
    )
    ap.add_argument("-o", "--output", required=True, help="Output .mdv path")
    ap.add_argument("--pbf", help="Local OSM PBF (requires osmium)")
    ap.add_argument("--geojson", action="append", default=[], help="GeoJSON file(s)")
    ap.add_argument(
        "--overpass",
        action="store_true",
        help="Force Overpass fetch (default if no --pbf/--geojson)",
    )
    ap.add_argument("--buildings", action="store_true", help="Include building footprints")
    ap.add_argument(
        "--scale",
        type=float,
        default=0,
        help="Coord scale (default: auto from bbox, typically 1e5)",
    )
    ap.add_argument(
        "--simplify",
        type=float,
        default=0.00008,
        help="Douglas-Peucker tolerance in degrees",
    )
    ap.add_argument(
        "--densify",
        type=float,
        default=0.00025,
        help="Max edge length in degrees before densify (smooth curves)",
    )
    ap.add_argument("--max-pts", type=int, default=550000)
    args = ap.parse_args()

    lon0, lat0, lon1, lat1 = args.bbox
    if lon0 >= lon1 or lat0 >= lat1:
        print("Invalid bbox", file=sys.stderr)
        sys.exit(1)

    # Auto scale so int32 coords are fine-grained but safe
    m = max(abs(lon0), abs(lat0), abs(lon1), abs(lat1), 1.0)
    scale = args.scale or min(200000.0, 2e9 / m / 2)

    b = Builder(
        (lon0, lat0, lon1, lat1),
        scale,
        args.simplify,
        args.densify,
        max_pts=args.max_pts,
    )

    if args.pbf:
        build_from_pbf(b, args.pbf, (lon0, lat0, lon1, lat1), buildings=args.buildings)
    elif args.geojson:
        for g in args.geojson:
            print(f"Loading {g}...")
            build_from_geojson(b, g)
    else:
        # Overpass default
        for attempt in range(3):
            try:
                data = overpass_query(
                    (lon0, lat0, lon1, lat1), buildings=args.buildings
                )
                break
            except Exception as e:
                print(f"Overpass attempt {attempt + 1} failed: {e}")
                if attempt == 2:
                    print(
                        "Tip: download https://download.geofabrik.de/north-america/us/idaho-latest.osm.pbf\n"
                        "     then: python tools/gen_newmap.py --pbf idaho-latest.osm.pbf --bbox ... -o out.mdv",
                        file=sys.stderr,
                    )
                    sys.exit(1)
                time.sleep(5)
        build_from_overpass(b, data, buildings=args.buildings)

    if not b.feats and not b.labels:
        print("No features collected — empty pack not written.", file=sys.stderr)
        sys.exit(1)

    b.write(args.output)
    print("Done. Copy to SD: /meshdeck-maps/  then open NewMaps on the device.")
    print("Attribution: © OpenStreetMap contributors")


if __name__ == "__main__":
    main()
