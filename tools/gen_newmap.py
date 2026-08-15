#!/usr/bin/env python3
"""
Generate MeshDeck NewMaps packs (.mdv) from OpenStreetMap.

MDV2 is a tiled, zoom-banded vector basemap for the T-Deck (ESP32-S3):
  LOD0 overview  - stitched major roads + water + cities (always in RAM)
  LOD1 tiles     - residential / landuse / rail (paged from SD when zoomed)
  LOD2 tiles     - service / path / buildings (paged only when close)
  v3 feats       - each road carries name_id; stitch only same name/ref

A small bbox writes one .mdv. A large bbox is split into overlapping
name_00.mdv, name_01.mdv, ... files (not thousands of tile files -- FAT32
+ SPI SD hates that). The device loads every intersecting region's
overview at boot, then only the tiles that cover the current view.

Sources (pick one):
  A) Local .osm.pbf via `pip install osmium` (recommended)
  B) Overpass API -- no local OSM install (default if no --pbf/--geojson)
  C) Local GeoJSON FeatureCollection(s)

Examples -- Northern Idaho (Coeur d'Alene / Sandpoint / Palouse fringe):

  python tools/gen_newmap.py --pbf idaho-latest.osm.pbf \\
      --bbox -117.5 46.0 -115.5 49.0 \\
      -o sdcard/meshdeck-maps/northern-idaho.mdv

  python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 \\
      -o sdcard/meshdeck-maps/cda.mdv --buildings

Copy every generated .mdv onto the SD card under /meshdeck-maps/.
See docs/NEWMAPS.md for flags, install, and controls.

License: OpenStreetMap data is © OpenStreetMap contributors (ODbL).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys
import time
import urllib.parse
import urllib.request
from collections import defaultdict

MAGIC_V2 = 0x3256444D  # "MDV2"
VERSION = 3  # feats carry u16 name_id (1-based into names[])

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
NMF_DRIVEWAY = 0x02
NMF_SIGNED = 0x04  # I / US / state numbered route -- always LOD0

SERVICE_MINOR = {
    "driveway",
    "parking_aisle",
    "alley",
    "drive-through",
    "emergency_access",
}


def normalize_ref(ref):
    return (ref or "").upper().replace(" ", "").replace(".", "")


def classify_signed(ref, network, name=""):
    """I / US / state numbered routes -> (layer, min_scale) or None.

    OSM often retags a state highway as residential through town. The
    route relation (network=US:ID, ref=3) and way ref= are the source of
    truth for a continuous signed network.
    """
    net = (network or "").upper()
    r = normalize_ref(ref)
    nm = (name or "").upper()
    if not r and not net and not nm:
        return None
    # Interstate: I-90 / network=US:I  (not "ID 3")
    if (
        net == "US:I"
        or net.startswith("US:I;")
        or net.endswith(":I")
        or (len(r) >= 2 and r[0] == "I" and r[1].isdigit())
        or "INTERSTATE" in nm
    ):
        return NML_ROAD_MOTORWAY, 8
    if (
        net == "US:US"
        or "US:US" in net
        or r.startswith("US")
        or "UNITED STATES" in nm
        or nm.startswith("US ")
        or nm.startswith("U.S.")
        or "US HIGHWAY" in nm
        or "US ROUTE" in nm
    ):
        return NML_ROAD_PRIMARY, 8
    # State / provincial (US:ID, US:WA, ...) or any other signed ref
    if (
        net.startswith("US:")
        or net.startswith("CA:")
        or r
        or "STATE HIGHWAY" in nm
        or "STATE ROUTE" in nm
        or "STATE HWY" in nm
    ):
        return NML_ROAD_SECONDARY, 8
    return None


def classify_highway(tags):
    """Return (layer, min_scale, flags) or None."""
    flags = 0
    signed = classify_signed(
        tags.get("ref"), tags.get("network"), tags.get("name") or ""
    )
    hw = tags.get("highway")
    if signed:
        layer, ms = signed
        flags |= NMF_SIGNED
        # Keep motorway/trunk class if the way itself is higher
        if hw in HIGHWAY_MAP:
            hw_layer, _hw_ms = HIGHWAY_MAP[hw]
            if hw_layer > layer:
                layer = hw_layer
        return layer, ms, flags
    if hw not in HIGHWAY_MAP:
        return None
    layer, ms = HIGHWAY_MAP[hw]
    if hw == "service":
        svc = tags.get("service") or ""
        if svc in SERVICE_MINOR:
            flags |= NMF_DRIVEWAY
            ms = 2048 if svc == "driveway" else 1536
    return layer, ms, flags


def ascii_clip(s, n):
    s = s or ""
    return "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in s)[:n]


_ROAD_ABBREV = (
    (" Road", " Rd"),
    (" Street", " St"),
    (" Avenue", " Ave"),
    (" Drive", " Dr"),
    (" Lane", " Ln"),
    (" Court", " Ct"),
    (" Boulevard", " Blvd"),
    (" Highway", " Hwy"),
    (" Place", " Pl"),
    (" Circle", " Cir"),
    (" Trail", " Trl"),
    (" North", " N"),
    (" South", " S"),
    (" East", " E"),
    (" West", " W"),
)


def fit_road_name(name, n=19):
    """ASCII clip; abbreviate common suffixes so the name still fits."""
    name = ascii_clip(name, 80)
    if len(name) <= n:
        return name
    out = name
    for long, short in _ROAD_ABBREV:
        if long in out:
            out = out.replace(long, short)
            if len(out) <= n:
                return out
    return out[:n]


def road_label(tags):
    """(name, ref, place) from OSM tags. Empty strings if unnamed."""
    if not tags:
        return "", "", ""
    name = fit_road_name(tags.get("name") or tags.get("name:en") or "", 19)
    ref = ascii_clip(tags.get("ref") or "", 9)
    city = tags.get("addr:city") or tags.get("is_in:city") or ""
    state = tags.get("addr:state") or tags.get("is_in:state") or ""
    place = ""
    if city and state:
        place = ascii_clip(city + ", " + state, 15)
    elif city:
        place = ascii_clip(city, 15)
    elif state:
        place = ascii_clip(state, 15)
    return name, ref, place

# highway=* → (layer, min_scale px/deg)
HIGHWAY_MAP = {
    "motorway": (NML_ROAD_MOTORWAY, 16),
    "motorway_link": (NML_ROAD_MOTORWAY, 96),
    "trunk": (NML_ROAD_TRUNK, 16),
    "trunk_link": (NML_ROAD_TRUNK, 96),
    "primary": (NML_ROAD_PRIMARY, 24),
    "primary_link": (NML_ROAD_PRIMARY, 128),
    "secondary": (NML_ROAD_SECONDARY, 64),
    "secondary_link": (NML_ROAD_SECONDARY, 192),
    "tertiary": (NML_ROAD_TERTIARY, 192),
    "tertiary_link": (NML_ROAD_TERTIARY, 384),
    "unclassified": (NML_ROAD_RESIDENTIAL, 384),
    "residential": (NML_ROAD_RESIDENTIAL, 512),
    "living_street": (NML_ROAD_RESIDENTIAL, 768),
    "road": (NML_ROAD_RESIDENTIAL, 512),
    "service": (NML_ROAD_SERVICE, 1024),
    "track": (NML_ROAD_PATH, 1024),
    "path": (NML_ROAD_PATH, 1536),
    "footway": (NML_ROAD_PATH, 2048),
    "cycleway": (NML_ROAD_PATH, 1536),
    "bridleway": (NML_ROAD_PATH, 2048),
    "steps": (NML_ROAD_PATH, 2048),
    "pedestrian": (NML_ROAD_PATH, 1536),
}

WATERWAY_LINE = {
    "river": 16,
    "canal": 64,
    "stream": 512,
    "drain": 1024,
    "ditch": 1536,
}

LANDUSE_MAP = {
    "residential": NML_LANDUSE_RESIDENTIAL,
    "industrial": NML_LANDUSE_INDUSTRIAL,
    "commercial": NML_LANDUSE_RESIDENTIAL,
    "retail": NML_LANDUSE_RESIDENTIAL,
    "forest": NML_LANDUSE_FOREST,
    "wood": NML_LANDUSE_FOREST,
    # farmland/meadow/grass omitted -- Palouse fields ate the 65k tile
    # budget and streets were dropped
    "recreation_ground": NML_LANDUSE_PARK,
    "village_green": NML_LANDUSE_PARK,
    "allotments": NML_LANDUSE_PARK,
    "cemetery": NML_LANDUSE_PARK,
}

LEISURE_PARK = {"park", "nature_reserve", "garden", "pitch", "golf_course", "playground"}
NATURAL_WATER = {"water", "bay", "strait", "lake", "pond", "reservoir"}
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

# Ingest priority: major roads first so they never lose a budget fight
LAYER_PRIORITY = {
    NML_ROAD_MOTORWAY: 0,
    NML_ROAD_TRUNK: 1,
    NML_ROAD_PRIMARY: 2,
    NML_ROAD_SECONDARY: 3,
    NML_WATER_AREA: 4,
    NML_WATER_LINE: 5,
    NML_ROAD_TERTIARY: 6,
    NML_RAIL: 7,
    NML_LANDUSE_PARK: 8,
    NML_LANDUSE_FOREST: 9,
    NML_LANDUSE_RESIDENTIAL: 10,
    NML_LANDUSE_INDUSTRIAL: 11,
    NML_ROAD_RESIDENTIAL: 12,
    NML_ROAD_SERVICE: 13,
    NML_ROAD_PATH: 14,
    NML_BUILDING: 15,
}

# LOD0 stays in RAM. LOD1/2 are tiled and paged.
LOD0_LAYERS = {
    NML_ROAD_MOTORWAY,
    NML_ROAD_TRUNK,
    NML_ROAD_PRIMARY,
    NML_ROAD_SECONDARY,
    NML_ROAD_TERTIARY,
    NML_WATER_AREA,
}
LOD2_LAYERS = {
    NML_ROAD_PATH,
    NML_ROAD_SERVICE,
    NML_BUILDING,
}

LOD0_MIN_SCALE = 0
LOD1_MIN_SCALE = 384
LOD2_MIN_SCALE = 1024


def area_span_deg(coords):
    if not coords:
        return 0.0
    xs = [p[0] for p in coords]
    ys = [p[1] for p in coords]
    return max(max(xs) - min(xs), max(ys) - min(ys))


def lod_for(layer: int, min_scale: int, flags: int = 0, coords=None) -> int:
    # Signed I/US/state routes always stay in the RAM overview.
    if flags & NMF_SIGNED:
        return 0
    if layer in (NML_ROAD_MOTORWAY, NML_ROAD_TRUNK, NML_ROAD_PRIMARY):
        return 0
    if layer == NML_WATER_AREA:
        # Large lakes stay in RAM. Ponds / small reservoirs (Spring Valley)
        # go to LOD1 so the 100k overview cap cannot drop them.
        return 0 if area_span_deg(coords) >= 0.03 else 1
    if layer == NML_WATER_LINE and min_scale <= 48:
        return 0  # rivers / canals ride with the overview
    if layer in LOD2_LAYERS:
        return 2
    if layer == NML_WATER_LINE and min_scale >= 128:
        return 2  # streams / drains
    # Unsigned secondary/tertiary are local collectors -- not the
    # zoomed-out highway network. They were eating the overview budget
    # and knocking state routes off the map.
    return 1


def simplify_tol_for_lod(lod: int, base: float) -> float:
    if lod == 0:
        return max(base * 3.0, 0.00022)
    if lod == 2:
        return max(base * 0.5, 0.00003)
    return base


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def douglas_peucker(coords, tol):
    """coords: list of (lon, lat). tol in degrees."""
    if len(coords) <= 2 or tol <= 0:
        return coords

    def dist_point_seg(p, a, b):
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


def _clip_t(p, q, t0, t1):
    if p == 0:
        return (t0, t1) if q >= 0 else None
    t = q / p
    if p < 0:
        if t > t1:
            return None
        if t > t0:
            t0 = t
    else:
        if t < t0:
            return None
        if t < t1:
            t1 = t
    return t0, t1


def liang_barsky(a, b, bbox):
    """Clip segment a->b to [lon0,lat0,lon1,lat1]. Return (p,q) or None."""
    lon0, lat0, lon1, lat1 = bbox
    x0, y0 = a
    x1, y1 = b
    dx, dy = x1 - x0, y1 - y0
    t0, t1 = 0.0, 1.0
    for p, q in (
        (-dx, x0 - lon0),
        (dx, lon1 - x0),
        (-dy, y0 - lat0),
        (dy, lat1 - y0),
    ):
        r = _clip_t(p, q, t0, t1)
        if r is None:
            return None
        t0, t1 = r
    return (x0 + t0 * dx, y0 + t0 * dy), (x0 + t1 * dx, y0 + t1 * dy)


def clip_linestring(coords, bbox):
    """Proper clip: keep crossing segments (interpolated), split into pieces."""
    if len(coords) < 2:
        return []
    pieces = []
    cur = []
    for i in range(len(coords) - 1):
        seg = liang_barsky(coords[i], coords[i + 1], bbox)
        if seg is None:
            if len(cur) >= 2:
                pieces.append(cur)
            cur = []
            continue
        p, q = seg
        if not cur:
            cur = [p, q]
        else:
            if math.hypot(cur[-1][0] - p[0], cur[-1][1] - p[1]) > 1e-12:
                if len(cur) >= 2:
                    pieces.append(cur)
                cur = [p, q]
            else:
                cur.append(q)
    if len(cur) >= 2:
        pieces.append(cur)
    return pieces


def sutherland_hodgman(coords, bbox):
    lon0, lat0, lon1, lat1 = bbox

    def clip_edge(pts, inside, intersect):
        if not pts:
            return []
        out = []
        prev = pts[-1]
        prev_in = inside(prev)
        for cur in pts:
            cur_in = inside(cur)
            if cur_in:
                if not prev_in:
                    out.append(intersect(prev, cur))
                out.append(cur)
            elif prev_in:
                out.append(intersect(prev, cur))
            prev, prev_in = cur, cur_in
        return out

    def ix(a, b, x):
        ax, ay = a
        bx, by = b
        if bx == ax:
            return (x, ay)
        t = (x - ax) / (bx - ax)
        return (x, ay + t * (by - ay))

    def iy(a, b, y):
        ax, ay = a
        bx, by = b
        if by == ay:
            return (ax, y)
        t = (y - ay) / (by - ay)
        return (ax + t * (bx - ax), y)

    pts = list(coords)
    if len(pts) >= 2 and pts[0] == pts[-1]:
        pts = pts[:-1]
    pts = clip_edge(pts, lambda p: p[0] >= lon0, lambda a, b: ix(a, b, lon0))
    pts = clip_edge(pts, lambda p: p[0] <= lon1, lambda a, b: ix(a, b, lon1))
    pts = clip_edge(pts, lambda p: p[1] >= lat0, lambda a, b: iy(a, b, lat0))
    pts = clip_edge(pts, lambda p: p[1] <= lat1, lambda a, b: iy(a, b, lat1))
    return pts


def qkey(p, q=1e5):
    return (int(round(p[0] * q)), int(round(p[1] * q)))


def is_ring(coords):
    return len(coords) >= 4 and qkey(coords[0]) == qkey(coords[-1])


def classify_area_tags(tags):
    """Closed-area layer from OSM tags, or None."""
    if not tags:
        return None
    if (
        tags.get("natural") in NATURAL_WATER
        or tags.get("water")
        or tags.get("waterway") == "riverbank"
        or tags.get("landuse") == "reservoir"
    ):
        return NML_WATER_AREA, 24
    if tags.get("natural") in NATURAL_WOOD or tags.get("landuse") == "forest":
        return NML_LANDUSE_FOREST, 48
    if tags.get("leisure") in LEISURE_PARK:
        return NML_LANDUSE_PARK, 48
    if tags.get("landuse") in LANDUSE_MAP:
        return LANDUSE_MAP[tags["landuse"]], 48
    return None


def add_area_rings(b, outers, layer, ms):
    """Add outer rings as filled areas. Already-closed rings stay intact;
    open shoreline pieces are stitched then closed."""
    if not outers:
        return
    closed, open_ = [], []
    for coords in outers:
        if len(coords) < 3:
            continue
        if is_ring(coords):
            closed.append(coords)
        else:
            open_.append(coords)
    for ring in closed:
        b.add_line(ring, layer, ms, closed=True)
    if open_:
        for line in stitch_lines(open_, max_pts=8000):
            if len(line) >= 3:
                b.add_line(line, layer, ms, closed=True)


def stitch_lines(polylines, max_pts=1800):
    """Merge OSM ways that share endpoints so highways are continuous."""
    chains = {}
    nid = 0
    for p in polylines:
        if len(p) < 2:
            continue
        chains[nid] = list(p)
        nid += 1
    if len(chains) < 2:
        return list(chains.values())

    endmap = defaultdict(set)

    def add_ends(i):
        c = chains[i]
        endmap[qkey(c[0])].add((i, True))
        endmap[qkey(c[-1])].add((i, False))

    def rm_ends(i):
        c = chains[i]
        endmap[qkey(c[0])].discard((i, True))
        endmap[qkey(c[-1])].discard((i, False))

    for i in chains:
        add_ends(i)

    def join(i, i_at_start, j, j_at_start):
        a = chains[i]
        b = chains[j]
        if len(a) + len(b) - 1 > max_pts:
            return False
        rm_ends(i)
        rm_ends(j)
        if i_at_start:
            a.reverse()
        if not j_at_start:
            b.reverse()
        if qkey(a[-1]) == qkey(b[0]):
            a.extend(b[1:])
        else:
            a.extend(b)
        del chains[j]
        add_ends(i)
        return True

    progressed = True
    while progressed:
        progressed = False
        for items in list(endmap.values()):
            if len(items) < 2:
                continue
            lst = list(items)
            (i, is_), (j, js) = lst[0], lst[1]
            if i == j or i not in chains or j not in chains:
                continue
            if join(i, is_, j, js):
                progressed = True
                break
    return list(chains.values())


def feat_bbox(coords):
    xs = [p[0] for p in coords]
    ys = [p[1] for p in coords]
    return min(xs), min(ys), max(xs), max(ys)


def feat_overlaps(coords, bbox):
    if not coords:
        return False
    minx, miny, maxx, maxy = feat_bbox(coords)
    lon0, lat0, lon1, lat1 = bbox
    return not (maxx < lon0 or minx > lon1 or maxy < lat0 or miny > lat1)


def count_pts_in_bbox(feats, bbox):
    n = 0
    for f in feats:
        if feat_overlaps(f.coords, bbox):
            n += len(f.coords)
    return n


def split_bbox_by_pts(bbox, feats, max_pts, max_cells=16, min_deg=0.2, depth=0):
    """Adaptive split: denser areas become smaller files."""
    lon0, lat0, lon1, lat1 = bbox
    n = count_pts_in_bbox(feats, bbox)
    w, h = lon1 - lon0, lat1 - lat0
    if (
        n <= max_pts
        or max_cells <= 1
        or depth >= 6
        or min(w, h) < min_deg
    ):
        return [bbox]
    left_cells = (max_cells + 1) // 2
    right_cells = max_cells - left_cells
    if w >= h:
        mid = 0.5 * (lon0 + lon1)
        a = (lon0, lat0, mid, lat1)
        b = (mid, lat0, lon1, lat1)
    else:
        mid = 0.5 * (lat0 + lat1)
        a = (lon0, lat0, lon1, mid)
        b = (lon0, mid, lon1, lat1)
    return split_bbox_by_pts(
        a, feats, max_pts, left_cells, min_deg, depth + 1
    ) + split_bbox_by_pts(b, feats, max_pts, right_cells, min_deg, depth + 1)


def clip_feats_to_cell(feats, bbox, pad):
    lon0, lat0, lon1, lat1 = bbox
    box = (lon0 - pad, lat0 - pad, lon1 + pad, lat1 + pad)
    out = []
    for f in feats:
        if f.flags & NMF_CLOSED:
            pts = sutherland_hodgman(f.coords, box)
            if len(pts) >= 3:
                if pts[0] != pts[-1]:
                    pts = pts + [pts[0]]
                out.append(f.clone(pts))
        else:
            for piece in clip_linestring(f.coords, box):
                if len(piece) >= 2:
                    out.append(f.clone(piece))
    return out


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------

class Feat:
    __slots__ = ("layer", "flags", "min_scale", "coords", "name", "ref", "place", "name_id")

    def __init__(self, layer, flags, min_scale, coords, name="", ref="", place="", name_id=0):
        self.layer = layer
        self.flags = flags
        self.min_scale = min_scale
        self.coords = coords
        self.name = name or ""
        self.ref = ref or ""
        self.place = place or ""
        self.name_id = name_id

    def clone(self, coords):
        return Feat(
            self.layer,
            self.flags,
            self.min_scale,
            coords,
            name=self.name,
            ref=self.ref,
            place=self.place,
            name_id=self.name_id,
        )


class Builder:
    def __init__(self, bbox, scale, simplify_tol):
        self.bbox = bbox  # lon0, lat0, lon1, lat1
        self.scale = scale
        self.tol = simplify_tol
        self.raw = []  # Feat
        self.labels = []

    def add_line(self, coords, layer, min_scale, closed=False, extra_flags=0, tags=None):
        if len(coords) < 2:
            return
        name, ref, place = road_label(tags)
        lon0, lat0, lon1, lat1 = self.bbox
        margin = 0.03
        clip_box = (lon0 - margin, lat0 - margin, lon1 + margin, lat1 + margin)
        if closed:
            pts = sutherland_hodgman(coords, clip_box)
            if len(pts) < 3:
                return
            pts = douglas_peucker(pts, self.tol)
            if len(pts) < 3:
                return
            if pts[0] != pts[-1]:
                pts = pts + [pts[0]]
            self.raw.append(
                Feat(layer, NMF_CLOSED | extra_flags, int(min_scale), pts, name, ref, place)
            )
            return
        for piece in clip_linestring(coords, clip_box):
            piece = douglas_peucker(piece, self.tol)
            if len(piece) >= 2:
                self.raw.append(
                    Feat(layer, extra_flags, int(min_scale), piece, name, ref, place)
                )

    def assign_places(self, names):
        places = [(la, lo, k, nm) for la, lo, k, _ms, nm in self.labels if k <= 2]
        if not places:
            return names
        out = []
        for lat, lon, layer, flags, name, ref, place in names:
            if place:
                out.append((lat, lon, layer, flags, name, ref, place))
                continue
            best, best_s = "", 1e9
            for la, lo, k, nm in places:
                d = (la - lat) * (la - lat) + (lo - lon) * (lo - lon)
                s = d + k * 0.0004
                if s < best_s:
                    best_s, best = s, nm
            if best and best_s < 0.25:
                place = best[:15]
            out.append((lat, lon, layer, flags, name, ref, place))
        return out

    def _bind_names(self, by_lod, max_names=8000):
        """Assign per-file name_id on each feat. Returns unique name rows."""
        recs = {}
        for lod in by_lod:
            for f in lod:
                if not f.name and not f.ref:
                    f.name_id = 0
                    continue
                key = (f.name, f.ref)
                mid = f.coords[len(f.coords) // 2]
                lat, lon = mid[1], mid[0]
                rank = (0 if (f.flags & NMF_SIGNED) else 1, -f.layer)
                if key not in recs:
                    recs[key] = {
                        "lat": lat,
                        "lon": lon,
                        "layer": f.layer,
                        "flags": f.flags,
                        "name": f.name,
                        "ref": f.ref,
                        "place": f.place,
                        "rank": rank,
                    }
                else:
                    r = recs[key]
                    if rank < r["rank"]:
                        r["layer"] = f.layer
                        r["flags"] = f.flags
                        r["rank"] = rank
                    if not r["place"] and f.place:
                        r["place"] = f.place
        ordered = sorted(recs.values(), key=lambda r: r["rank"])[:max_names]
        idmap = {}
        names = []
        for i, r in enumerate(ordered):
            idmap[(r["name"], r["ref"])] = i + 1
            names.append(
                (r["lat"], r["lon"], r["layer"], r["flags"], r["name"], r["ref"], r["place"])
            )
        for lod in by_lod:
            for f in lod:
                if f.name or f.ref:
                    f.name_id = idmap.get((f.name, f.ref), 0)
                else:
                    f.name_id = 0
        return names

    def add_label(self, lon, lat, kind, min_scale, name):
        lon0, lat0, lon1, lat1 = self.bbox
        if not (lon0 <= lon <= lon1 and lat0 <= lat <= lat1):
            return
        name = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in name)[:19]
        if not name:
            return
        self.labels.append((lat, lon, kind, min_scale, name))

    def _stitch_and_budget(self, max_overview_pts, max_tile_pts):
        groups = defaultdict(list)
        closed = []
        for f in self.raw:
            if f.flags & NMF_CLOSED:
                closed.append(f)
            else:
                if f.flags & NMF_SIGNED or (
                    NML_ROAD_PATH <= f.layer <= NML_ROAD_MOTORWAY
                ):
                    # Stitch only pieces of the SAME named road. Mixing
                    # White Pine Flats Rd into Pleasant Hill Rd made
                    # long-press pick the wrong street at every junction.
                    groups[(f.layer, 0, f.flags, f.name, f.ref)].append(f)
                else:
                    groups[(f.layer, f.min_scale, f.flags, "", "")].append(f)

        stitched = []
        for (layer, ms, flags, name, ref), feats in groups.items():
            signed = bool(flags & NMF_SIGNED)
            cap = 8000 if signed else (4000 if layer >= NML_ROAD_PRIMARY else 1800)
            lines = stitch_lines([f.coords for f in feats], max_pts=cap)
            keep_ms = min(f.min_scale for f in feats) if signed or ms == 0 else ms
            place = next((x.place for x in feats if x.place), "")
            for coords in lines:
                stitched.append(
                    Feat(layer, flags, keep_ms, coords, name=name, ref=ref, place=place)
                )
        all_feats = stitched + closed
        all_feats.sort(
            key=lambda f: (
                0 if (f.flags & NMF_SIGNED) else 1,
                LAYER_PRIORITY.get(f.layer, 99),
                f.min_scale,
            )
        )

        by_lod = [[], [], []]
        pts_used = [0, 0, 0]
        budgets = [max_overview_pts, max_tile_pts, max_tile_pts]
        dropped = [0, 0, 0]
        for f in all_feats:
            lod = lod_for(f.layer, f.min_scale, f.flags, f.coords)
            n = len(f.coords)
            # Signed routes get first claim on overview RAM; never drop them
            # for unsigned clutter.
            signed = bool(f.flags & NMF_SIGNED)
            if not signed and pts_used[lod] + n > budgets[lod]:
                dropped[lod] += 1
                continue
            if signed and pts_used[lod] + n > budgets[lod] * 2:
                dropped[lod] += 1
                continue
            if lod == 0 and not (f.flags & NMF_CLOSED):
                tol = simplify_tol_for_lod(0, self.tol)
                if signed:
                    tol = max(self.tol * 1.5, 0.00012)
                f.coords = douglas_peucker(f.coords, tol)
                if len(f.coords) < 2:
                    continue
                n = len(f.coords)
            pts_used[lod] += n
            by_lod[lod].append(f)
        return by_lod, pts_used, dropped

    def write(self, path, tile_deg_lod1, tile_deg_lod2, max_overview_pts, max_tile_pts,
              max_file_pts=120000, overlap_deg=0.03, max_files=16):
        by_lod, pts_used, dropped = self._stitch_and_budget(max_overview_pts, max_tile_pts)
        for lod in range(3):
            print(
                f"  LOD{lod}: {len(by_lod[lod])} feats, {pts_used[lod]} pts"
                + (f" (dropped {dropped[lod]} over budget)" if dropped[lod] else "")
            )
        all_feats = by_lod[0] + by_lod[1] + by_lod[2]
        cells = split_bbox_by_pts(self.bbox, all_feats, max_file_pts, max_files)
        print(f"  {len(cells)} region file(s) (max {max_file_pts} pts, "
              f"{overlap_deg} deg overlap)")
        if len(cells) == 1:
            names = self.assign_places(self._bind_names(by_lod))
            self._emit_mdv2(path, self.bbox, by_lod, self.labels, names,
                            tile_deg_lod1, tile_deg_lod2)
            return
        stem, ext = os.path.splitext(path)
        if not ext:
            ext = ".mdv"
        for i, cell in enumerate(cells):
            clipped = [clip_feats_to_cell(by_lod[k], cell, overlap_deg) for k in range(3)]
            clon0, clat0, clon1, clat1 = cell
            labs = [
                L for L in self.labels
                if clat0 <= L[0] <= clat1 and clon0 <= L[1] <= clon1
            ]
            names = self.assign_places(self._bind_names(clipped))
            outp = f"{stem}_{i:02d}{ext}"
            print(f"  region {i}: {clat0:.3f}..{clat1:.3f} {clon0:.3f}..{clon1:.3f}")
            self._emit_mdv2(outp, cell, clipped, labs, names, tile_deg_lod1, tile_deg_lod2)

    def _emit_mdv2(self, path, bbox, by_lod, labels, names, tile_deg_lod1, tile_deg_lod2):
        lon0, lat0, lon1, lat1 = bbox

        # --- pack blobs ---
        def pack_key(f):
            # Roads first so landuse cannot steal the 65k-per-tile cap.
            if NML_ROAD_PATH <= f.layer <= NML_ROAD_MOTORWAY:
                return (0, -f.layer, f.min_scale)
            if f.layer == NML_RAIL:
                return (1, 0, 0)
            if f.layer == NML_WATER_LINE:
                return (2, 0, 0)
            if f.flags & NMF_CLOSED:
                return (4, LAYER_PRIORITY.get(f.layer, 99), 0)
            return (3, LAYER_PRIORITY.get(f.layer, 99), f.min_scale)

        def pack_blob(feats, max_pts=10**9):
            feats = sorted(feats, key=pack_key)
            pts = []
            frec = []
            skipped = 0
            for f in feats:
                if len(pts) + len(f.coords) > max_pts:
                    skipped += 1
                    continue
                start = len(pts)
                for lon, lat in f.coords:
                    pts.append(
                        (int(round(lat * self.scale)), int(round(lon * self.scale)))
                    )
                count = len(pts) - start
                if count < 2:
                    del pts[start:]
                    continue
                nid = int(getattr(f, "name_id", 0) or 0)
                if nid < 0:
                    nid = 0
                if nid > 65535:
                    nid = 65535
                frec.append(
                    (f.layer, f.flags, min(int(f.min_scale), 65535), start, count, nid)
                )
            blob = bytearray()
            for lat_s, lon_s in pts:
                blob += struct.pack("<ii", lat_s, lon_s)
            for rec in frec:
                blob += struct.pack(
                    "<BBHIIH",
                    rec[0] & 0xFF,
                    rec[1] & 0xFF,
                    rec[2],
                    rec[3],
                    rec[4],
                    rec[5] & 0xFFFF,
                )
            return bytes(blob), len(pts), len(frec), skipped

        ov_blob, ov_pts, ov_feats, _sk = pack_blob(by_lod[0])

        def tile_grid(feats, tile_deg):
            w = max(lon1 - lon0, 1e-6)
            h = max(lat1 - lat0, 1e-6)
            tiles = []
            nx = ny = 1
            tw = w
            th = h
            for _try in range(6):
                nx = max(1, int(math.ceil(w / tile_deg)))
                ny = max(1, int(math.ceil(h / tile_deg)))
                tw = w / nx
                th = h / ny
                pad = min(tw, th) * 0.08
                buckets = [[[] for _ in range(nx)] for _ in range(ny)]
                for f in feats:
                    minx, miny, maxx, maxy = feat_bbox(f.coords)
                    tx0 = int((minx - lon0) / tw)
                    tx1 = int((maxx - lon0) / tw)
                    ty0 = int((miny - lat0) / th)
                    ty1 = int((maxy - lat0) / th)
                    tx0 = max(0, min(nx - 1, tx0))
                    tx1 = max(0, min(nx - 1, tx1))
                    ty0 = max(0, min(ny - 1, ty0))
                    ty1 = max(0, min(ny - 1, ty1))
                    for ty in range(ty0, ty1 + 1):
                        for tx in range(tx0, tx1 + 1):
                            box = (
                                lon0 + tx * tw - pad,
                                lat0 + ty * th - pad,
                                lon0 + (tx + 1) * tw + pad,
                                lat0 + (ty + 1) * th + pad,
                            )
                            if f.flags & NMF_CLOSED:
                                pts = sutherland_hodgman(f.coords, box)
                                if len(pts) >= 3:
                                    if pts[0] != pts[-1]:
                                        pts = pts + [pts[0]]
                                    buckets[ty][tx].append(f.clone(pts))
                            else:
                                for piece in clip_linestring(f.coords, box):
                                    if len(piece) >= 2:
                                        buckets[ty][tx].append(f.clone(piece))
                biggest = 0
                for ty in range(ny):
                    for tx in range(nx):
                        n = sum(len(f.coords) for f in buckets[ty][tx])
                        if n > biggest:
                            biggest = n
                if biggest <= 50000 or tile_deg <= 0.08:
                    tiles = []
                    skip_n = 0
                    for ty in range(ny):
                        for tx in range(nx):
                            blob, npts, nfeats, sk = pack_blob(
                                buckets[ty][tx], max_pts=65535
                            )
                            skip_n += sk
                            tiles.append((blob, npts, nfeats))
                    if skip_n:
                        print(f"    warn: dropped {skip_n} feats over 65k tile cap")
                    break
                tile_deg *= 0.5
            return nx, ny, tw, th, tiles

        lod1 = tile_grid(by_lod[1], tile_deg_lod1)
        lod2 = tile_grid(by_lod[2], tile_deg_lod2)

        name_blob = bytearray()
        # names[] order is name_id 1..N from _bind_names -- do not reorder.
        for lat, lon, layer, flags, name, ref, place in names:
            nb = (name or "").encode("ascii", "replace")[:19]
            nb = nb + b"\0" * (20 - len(nb))
            rb = (ref or "").encode("ascii", "replace")[:9]
            rb = rb + b"\0" * (10 - len(rb))
            pb = (place or "").encode("ascii", "replace")[:15]
            pb = pb + b"\0" * (16 - len(pb))
            name_blob += struct.pack(
                "<iiBB20s10s16s",
                int(round(lat * self.scale)),
                int(round(lon * self.scale)),
                layer & 0xFF,
                flags & 0xFF,
                nb,
                rb,
                pb,
            )

        label_blob = bytearray()
        for lat, lon, kind, min_scale, name in labels:
            msd = min(255, max(0, int(min_scale) // 4))
            nb = name.encode("ascii", "replace")[:19]
            nb = nb + b"\0" * (20 - len(nb))
            label_blob += struct.pack(
                "<iiBB20s",
                int(round(lat * self.scale)),
                int(round(lon * self.scale)),
                kind & 0xFF,
                msd,
                nb,
            )

        # Layout:
        #   header 64
        #   lod table 3 * 32
        #   lod1 index
        #   lod2 index
        #   labels
        #   overview blob
        #   tile blobs
        n_lods = 3
        hdr = 64
        lod_tab = 32 * n_lods
        idx1 = 12 * lod1[0] * lod1[1]
        idx2 = 12 * lod2[0] * lod2[1]
        off = hdr + lod_tab
        lod1_index_off = off
        off += idx1
        lod2_index_off = off
        off += idx2
        labels_off = off
        off += len(label_blob)
        names_off = off
        off += len(name_blob)
        overview_off = off
        off += len(ov_blob)

        tile_entries = []  # (off, nbytes, npts, nfeats) in lod1-then-lod2 order
        cur = off
        for blob, npts, nfeats in lod1[4] + lod2[4]:
            if npts == 0 or nfeats == 0:
                tile_entries.append((0, 0, 0, 0))
            else:
                tile_entries.append((cur, len(blob), npts, nfeats))
                cur += len(blob)

        def lod_rec(min_scale, nx, ny, tw, th, index_off, is_overview):
            if is_overview:
                nx = ny = 0
                tw = th = 0.0
                index_off = 0
            return struct.pack(
                "<HHHHffffII",
                min_scale,
                nx,
                ny,
                0,
                float(lon0),
                float(lat0),
                float(tw),
                float(th),
                index_off,
                0,
            )

        with open(path, "wb") as out:
            out.write(
                struct.pack(
                    "<IHH4f f BBH IIIII II",
                    MAGIC_V2,
                    VERSION,
                    0,
                    float(lat0),
                    float(lat1),
                    float(lon0),
                    float(lon1),
                    float(self.scale),
                    n_lods,
                    0,
                    len(labels),
                    labels_off,
                    overview_off,
                    len(ov_blob),
                    ov_pts,
                    ov_feats,
                    names_off if names else 0,
                    len(names),
                )
            )
            # header is 64 bytes: IHH + 4f + f + BBH + 5I + 2I = 60, plus 4 pad
            out.write(b"\0\0\0\0")
            if out.tell() != 64:
                raise RuntimeError(f"MDV2 header size {out.tell()}, expected 64")
            out.write(lod_rec(LOD0_MIN_SCALE, 0, 0, 0, 0, 0, True))
            out.write(
                lod_rec(LOD1_MIN_SCALE, lod1[0], lod1[1], lod1[2], lod1[3], lod1_index_off, False)
            )
            out.write(
                lod_rec(LOD2_MIN_SCALE, lod2[0], lod2[1], lod2[2], lod2[3], lod2_index_off, False)
            )
            n1 = lod1[0] * lod1[1]
            for off_, nb, np, nf in tile_entries[:n1]:
                out.write(struct.pack("<IIHH", off_, nb, min(np, 65535), min(nf, 65535)))
            for off_, nb, np, nf in tile_entries[n1:]:
                out.write(struct.pack("<IIHH", off_, nb, min(np, 65535), min(nf, 65535)))
            out.write(label_blob)
            out.write(name_blob)
            out.write(ov_blob)
            for blob, npts, nfeats in lod1[4] + lod2[4]:
                if npts and nfeats:
                    out.write(blob)

        kb = cur / 1024.0
        print(
            f"Wrote {path}: MDV2 overview {ov_pts} pts / {ov_feats} feats, "
            f"{len(labels)} labels, {len(names)} names, "
            f"LOD1 {lod1[0]}x{lod1[1]} @{tile_deg_lod1}deg, "
            f"LOD2 {lod2[0]}x{lod2[1]} @{tile_deg_lod2}deg, {kb:.0f} KB"
        )


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

def overpass_query(bbox, buildings=False, timeout=180):
    lon0, lat0, lon1, lat1 = bbox
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
        f'relation["type"="route"]["route"="road"]({bb});',
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
        url, data=data, headers={"User-Agent": "MeshDeck-gen_newmap/2.0"}
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

    consumed = set()
    for rel in relations:
        tags = rel.get("tags") or {}
        if tags.get("type") != "route" or tags.get("route") != "road":
            continue
        signed = classify_signed(
            tags.get("ref"), tags.get("network"), tags.get("name") or ""
        )
        if not signed:
            continue
        layer, ms = signed
        parts = []
        for mem in rel.get("members") or []:
            if mem.get("type") != "way":
                continue
            w = ways.get(mem.get("ref"))
            if not w:
                continue
            coords = way_coords(w)
            if len(coords) >= 2:
                parts.append(coords)
                consumed.add(mem.get("ref"))
        for line in stitch_lines(parts, max_pts=8000):
            b.add_line(coords=line, layer=layer, min_scale=ms, extra_flags=NMF_SIGNED, tags=tags)
        if parts:
            print(f"  route {tags.get('network','')} {tags.get('ref','')}: "
                  f"{len(parts)} members")

    for _wid, w in ways.items():
        tags = w.get("tags") or {}
        coords = way_coords(w)
        if len(coords) < 2:
            continue

        if "highway" in tags:
            if _wid in consumed:
                continue
            cls = classify_highway(tags)
            if cls:
                layer, ms, fl = cls
                b.add_line(coords, layer, ms, closed=False, extra_flags=fl, tags=tags)
            continue

        if tags.get("railway") in ("rail", "light_rail", "subway", "tram"):
            b.add_line(coords, NML_RAIL, 64, closed=False)
            continue

        if "waterway" in tags:
            if tags.get("waterway") == "riverbank" and is_ring(coords):
                b.add_line(coords, NML_WATER_AREA, 24, closed=True)
            else:
                ww = tags["waterway"]
                ms = WATERWAY_LINE.get(ww, 128)
                b.add_line(coords, NML_WATER_LINE, ms, closed=False)
            continue

        if tags.get("natural") in NATURAL_WATER or tags.get("water") or tags.get("landuse") == "reservoir":
            if is_ring(coords):
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

    for rel in relations:
        tags = rel.get("tags") or {}
        layer = None
        ms = 32
        if (
            tags.get("natural") in NATURAL_WATER
            or tags.get("water")
            or (tags.get("type") == "multipolygon" and tags.get("natural") == "water")
        ):
            layer, ms = NML_WATER_AREA, 24
        elif tags.get("landuse") in LANDUSE_MAP:
            layer = LANDUSE_MAP[tags["landuse"]]
            ms = 48
        elif tags.get("leisure") in LEISURE_PARK:
            layer, ms = NML_LANDUSE_PARK, 48
        if layer is None:
            continue
        outers = []
        for mem in rel.get("members") or []:
            if mem.get("type") != "way" or mem.get("role") not in ("outer", ""):
                continue
            w = ways.get(mem.get("ref"))
            if not w:
                continue
            coords = way_coords(w)
            if len(coords) >= 3:
                outers.append(coords)
        add_area_rings(b, outers, layer, ms)


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
        cls = classify_highway(props)
        extra = 0
        if cls:
            layer, ms, extra = cls
        if gtype == "LineString":
            b.add_line(coords, layer, ms, closed=False, extra_flags=extra, tags=props)
        elif gtype == "MultiLineString":
            for line in coords:
                b.add_line(line, layer, ms, closed=False, extra_flags=extra, tags=props)
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
    pad = 0.05

    def in_bbox(lon, lat):
        return (lon0 - pad <= lon <= lon1 + pad) and (lat0 - pad <= lat <= lat1 + pad)

    hw_ways = {}  # id -> (coords, tags)
    area_ways = {}  # id -> coords  (untagged + water/landuse; for multipolygons)

    class WayPass(osmium.SimpleHandler):
        def __init__(self):
            super().__init__()
            self.n = 0

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
            coords = []
            try:
                for n in w.nodes:
                    if n.location.valid():
                        coords.append((n.location.lon, n.location.lat))
            except osmium.InvalidLocationError:
                return
            if len(coords) < 2:
                return
            if not any(in_bbox(lon, lat) for lon, lat in coords):
                return
            tags = {k: v for k, v in w.tags}
            if (
                not tags
                or "natural" in tags
                or "water" in tags
                or "waterway" in tags
                or "landuse" in tags
                or "leisure" in tags
            ):
                area_ways[w.id] = coords
            if "highway" in tags:
                hw_ways[w.id] = (coords, tags)
            elif tags.get("railway") in ("rail", "light_rail", "subway", "tram"):
                b.add_line(coords, NML_RAIL, 64, closed=False)
            elif "waterway" in tags:
                if tags.get("waterway") == "riverbank" and is_ring(coords):
                    b.add_line(coords, NML_WATER_AREA, 24, closed=True)
                else:
                    ms = WATERWAY_LINE.get(tags["waterway"], 128)
                    b.add_line(coords, NML_WATER_LINE, ms, closed=False)
            elif tags.get("natural") in NATURAL_WATER or "water" in tags or tags.get("landuse") == "reservoir":
                if is_ring(coords):
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
            self.n += 1
            if self.n % 5000 == 0:
                print(f"  ... {self.n} ways")

    class RelPass(osmium.SimpleHandler):
        def __init__(self):
            super().__init__()
            self.consumed = set()
            self.nrel = 0
            self.narea = 0

        def relation(self, r):
            if r.tags.get("type") == "route" and r.tags.get("route") == "road":
                signed = classify_signed(
                    r.tags.get("ref"), r.tags.get("network"), r.tags.get("name") or ""
                )
                if not signed:
                    return
                layer, ms = signed
                parts = []
                for m in r.members:
                    if m.type != "w":
                        continue
                    rec = hw_ways.get(m.ref)
                    if not rec:
                        continue
                    parts.append(rec[0])
                    self.consumed.add(m.ref)
                if not parts:
                    return
                rtags = {k: v for k, v in r.tags}
                for line in stitch_lines(parts, max_pts=8000):
                    b.add_line(line, layer, ms, extra_flags=NMF_SIGNED, tags=rtags)
                self.nrel += 1
                if self.nrel <= 30 or self.nrel % 25 == 0:
                    print(f"  route {r.tags.get('network','')} {r.tags.get('ref','')}: "
                          f"{len(parts)} members")
                return

            tags = {k: v for k, v in r.tags}
            classified = classify_area_tags(tags)
            if not classified:
                return
            if tags.get("type") not in (None, "", "multipolygon", "boundary"):
                return
            layer, ms = classified
            outers = []
            for m in r.members:
                if m.type != "w" or m.role == "inner":
                    continue
                coords = area_ways.get(m.ref)
                if coords is None and m.ref in hw_ways:
                    coords = hw_ways[m.ref][0]
                if coords and len(coords) >= 3:
                    outers.append(coords)
            if not outers:
                return
            add_area_rings(b, outers, layer, ms)
            self.narea += 1
            if self.narea <= 15 or self.narea % 50 == 0:
                print(f"  area {tags.get('name', '') or '(unnamed)'} "
                      f"layer={layer} outers={len(outers)}")

    print(f"Reading {pbf_path} pass 1 (ways)...")
    WayPass().apply_file(pbf_path, locations=True)
    print(f"  stored {len(hw_ways)} highway ways, {len(area_ways)} area-member ways")
    print(f"Reading {pbf_path} pass 2 (route + area relations)...")
    rp = RelPass()
    rp.apply_file(pbf_path, locations=False)
    print(f"  {rp.nrel} signed routes, {len(rp.consumed)} member ways, "
          f"{rp.narea} area relations")
    leftover = 0
    for wid, (coords, tags) in hw_ways.items():
        if wid in rp.consumed:
            continue
        cls = classify_highway(tags)
        if cls:
            layer, ms, fl = cls
            b.add_line(coords, layer, ms, extra_flags=fl, tags=tags)
            leftover += 1
    print(f"  {leftover} leftover highway ways")


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
        help="Douglas-Peucker tolerance in degrees (LOD1; LOD0 is coarser)",
    )
    ap.add_argument(
        "--tile-lod1",
        type=float,
        default=0.75,
        help="LOD1 tile size in degrees (residential / landuse)",
    )
    ap.add_argument(
        "--tile-lod2",
        type=float,
        default=0.25,
        help="LOD2 tile size in degrees (service / path / buildings)",
    )
    ap.add_argument(
        "--max-overview-pts",
        type=int,
        default=100000,
        help="Max vertices kept in the always-RAM overview (LOD0)",
    )
    ap.add_argument(
        "--max-tile-pts",
        type=int,
        default=800000,
        help="Max vertices across all tiles of one tiled LOD (file budget)",
    )
    ap.add_argument(
        "--max-file-pts",
        type=int,
        default=120000,
        help="Split into more .mdv files when a region exceeds this many verts",
    )
    ap.add_argument(
        "--overlap",
        type=float,
        default=0.03,
        help="Overlap in degrees between region files (seamless boundaries)",
    )
    args = ap.parse_args()

    lon0, lat0, lon1, lat1 = args.bbox
    if lon0 >= lon1 or lat0 >= lat1:
        print("Invalid bbox", file=sys.stderr)
        sys.exit(1)

    m = max(abs(lon0), abs(lat0), abs(lon1), abs(lat1), 1.0)
    scale = args.scale or min(200000.0, 2e9 / m / 2)

    b = Builder((lon0, lat0, lon1, lat1), scale, args.simplify)

    if args.pbf:
        build_from_pbf(b, args.pbf, (lon0, lat0, lon1, lat1), buildings=args.buildings)
    elif args.geojson:
        for g in args.geojson:
            print(f"Loading {g}...")
            build_from_geojson(b, g)
    else:
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
                        "Tip: download a Geofabrik PBF then:\n"
                        "     python tools/gen_newmap.py --pbf file.osm.pbf --bbox ... -o out.mdv",
                        file=sys.stderr,
                    )
                    sys.exit(1)
                time.sleep(5)
        build_from_overpass(b, data, buildings=args.buildings)

    if not b.raw and not b.labels:
        print("No features collected — empty pack not written.", file=sys.stderr)
        sys.exit(1)

    print(f"Collected {len(b.raw)} raw feats, {len(b.labels)} labels. Stitching / tiling...")
    b.write(
        args.output,
        args.tile_lod1,
        args.tile_lod2,
        args.max_overview_pts,
        args.max_tile_pts,
        max_file_pts=args.max_file_pts,
        overlap_deg=args.overlap,
    )
    print("Done. Copy to SD: /meshdeck-maps/  then open NewMaps on the device.")
    print("Attribution: (c) OpenStreetMap contributors")


if __name__ == "__main__":
    main()
