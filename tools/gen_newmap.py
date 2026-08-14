#!/usr/bin/env python3
"""
Generate MeshDeck NewMaps packs (.mdv) from OpenStreetMap.

MDV2 is a tiled, zoom-banded vector basemap for the T-Deck (ESP32-S3):
  LOD0 overview  - stitched major roads + water + cities (always in RAM)
  LOD1 tiles     - residential / landuse / rail (paged from SD when zoomed)
  LOD2 tiles     - service / path / buildings (paged only when close)

One .mdv file per region (not thousands of tile files — FAT32 + SPI SD
hates that). The device loads overview at boot, then only the tiles that
cover the current view for the active LOD (+ the coarser neighbour LOD).

Sources (pick one):
  A) Overpass API  — no local OSM install (default)
  B) Local GeoJSON FeatureCollection(s)
  C) Local .osm / .osm.pbf via optional `osmium` Python module

Examples — Northern Idaho (Coeur d'Alene / Sandpoint / Palouse fringe):

  python tools/gen_newmap.py --pbf idaho-latest.osm.pbf \\
      --bbox -117.5 46.0 -115.5 49.0 \\
      -o sdcard/meshdeck-maps/northern-idaho.mdv

  python tools/gen_newmap.py --bbox -117.0 47.5 -116.4 47.9 \\
      -o sdcard/meshdeck-maps/cda.mdv --buildings

Copy the .mdv onto the SD card under /meshdeck-maps/.

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

MAGIC_V2 = 0x3256444D  # "MDV2"
VERSION = 2

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

SERVICE_MINOR = {
    "driveway",
    "parking_aisle",
    "alley",
    "drive-through",
    "emergency_access",
}


def classify_highway(tags):
    """Return (layer, min_scale, flags) or None."""
    hw = tags.get("highway")
    if hw not in HIGHWAY_MAP:
        return None
    layer, ms = HIGHWAY_MAP[hw]
    flags = 0
    if hw == "service":
        svc = tags.get("service") or ""
        if svc in SERVICE_MINOR:
            flags |= NMF_DRIVEWAY
            ms = 2048 if svc == "driveway" else 1536
    # Numbered routes (I-90, US-95, ID-3) stay on the regional network even
    # when OSM tags a downtown stretch as residential/unclassified.
    ref = (tags.get("ref") or "").upper().replace(" ", "")
    net = (tags.get("network") or "").upper()
    if ref or net:
        interstate = (
            ref.startswith("I") and len(ref) > 1 and ref[1].isdigit()
        ) or "US:I" in net or net.endswith(":I")
        us_hwy = ref.startswith("US") or "US:US" in net
        if interstate and layer < NML_ROAD_TRUNK:
            layer, ms = NML_ROAD_MOTORWAY, 16
        elif us_hwy and layer < NML_ROAD_PRIMARY:
            layer, ms = NML_ROAD_PRIMARY, 16
        elif layer < NML_ROAD_SECONDARY and (ref or "US:" in net):
            layer, ms = NML_ROAD_SECONDARY, 16
        elif layer == NML_ROAD_TERTIARY:
            ms = min(ms, 24)
        elif layer == NML_ROAD_SECONDARY:
            ms = min(ms, 16)
    return layer, ms, flags

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
LOD1_MIN_SCALE = 512
LOD2_MIN_SCALE = 1024


def lod_for(layer: int, min_scale: int) -> int:
    if layer in LOD0_LAYERS:
        return 0
    if layer == NML_WATER_LINE and min_scale <= 48:
        return 0  # rivers / canals ride with the overview
    if layer in LOD2_LAYERS:
        return 2
    if layer == NML_WATER_LINE and min_scale >= 128:
        return 2  # streams / drains
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


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------

class Feat:
    __slots__ = ("layer", "flags", "min_scale", "coords")

    def __init__(self, layer, flags, min_scale, coords):
        self.layer = layer
        self.flags = flags
        self.min_scale = min_scale
        self.coords = coords


class Builder:
    def __init__(self, bbox, scale, simplify_tol):
        self.bbox = bbox  # lon0, lat0, lon1, lat1
        self.scale = scale
        self.tol = simplify_tol
        self.raw = []  # Feat
        self.labels = []

    def add_line(self, coords, layer, min_scale, closed=False, extra_flags=0):
        if len(coords) < 2:
            return
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
            self.raw.append(Feat(layer, NMF_CLOSED | extra_flags, int(min_scale), pts))
            return
        for piece in clip_linestring(coords, clip_box):
            piece = douglas_peucker(piece, self.tol)
            if len(piece) >= 2:
                self.raw.append(Feat(layer, extra_flags, int(min_scale), piece))

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
                # Stitch majors by layer only so a state route stays one line
                # through town (OSM splits at every intersection).
                if NML_ROAD_TERTIARY <= f.layer <= NML_ROAD_MOTORWAY:
                    groups[(f.layer, 0, f.flags)].append(f)
                else:
                    groups[(f.layer, f.min_scale, f.flags)].append(f)

        stitched = []
        for (layer, ms, flags), feats in groups.items():
            cap = 4000 if NML_ROAD_TERTIARY <= layer <= NML_ROAD_MOTORWAY else 1800
            lines = stitch_lines([f.coords for f in feats], max_pts=cap)
            keep_ms = ms
            if NML_ROAD_TERTIARY <= layer <= NML_ROAD_MOTORWAY:
                keep_ms = min(f.min_scale for f in feats)
            for coords in lines:
                stitched.append(Feat(layer, flags, keep_ms, coords))
        all_feats = stitched + closed
        all_feats.sort(key=lambda f: (LAYER_PRIORITY.get(f.layer, 99), f.min_scale))

        by_lod = [[], [], []]
        pts_used = [0, 0, 0]
        budgets = [max_overview_pts, max_tile_pts, max_tile_pts]
        dropped = [0, 0, 0]
        for f in all_feats:
            lod = lod_for(f.layer, f.min_scale)
            n = len(f.coords)
            if pts_used[lod] + n > budgets[lod]:
                dropped[lod] += 1
                continue
            # Extra simplify on overview so RAM stays small and roads stay smooth
            if lod == 0 and not (f.flags & NMF_CLOSED):
                f.coords = douglas_peucker(f.coords, simplify_tol_for_lod(0, self.tol))
                if len(f.coords) < 2:
                    continue
                n = len(f.coords)
            pts_used[lod] += n
            by_lod[lod].append(f)
        return by_lod, pts_used, dropped

    def write(self, path, tile_deg_lod1, tile_deg_lod2, max_overview_pts, max_tile_pts):
        lon0, lat0, lon1, lat1 = self.bbox
        by_lod, pts_used, dropped = self._stitch_and_budget(max_overview_pts, max_tile_pts)
        for lod in range(3):
            print(
                f"  LOD{lod}: {len(by_lod[lod])} feats, {pts_used[lod]} pts"
                + (f" (dropped {dropped[lod]} over budget)" if dropped[lod] else "")
            )

        # --- pack blobs ---
        def pack_blob(feats, max_pts=10**9):
            feats = sorted(feats, key=lambda f: (LAYER_PRIORITY.get(f.layer, 99), f.min_scale))
            pts = []
            frec = []
            for f in feats:
                if len(pts) + len(f.coords) > max_pts:
                    break
                start = len(pts)
                for lon, lat in f.coords:
                    pts.append(
                        (int(round(lat * self.scale)), int(round(lon * self.scale)))
                    )
                count = len(pts) - start
                if count < 2:
                    del pts[start:]
                    continue
                frec.append((f.layer, f.flags, min(int(f.min_scale), 65535), start, count))
            blob = bytearray()
            for lat_s, lon_s in pts:
                blob += struct.pack("<ii", lat_s, lon_s)
            for rec in frec:
                blob += struct.pack("<BBHII", rec[0] & 0xFF, rec[1] & 0xFF, rec[2], rec[3], rec[4])
            return bytes(blob), len(pts), len(frec)

        ov_blob, ov_pts, ov_feats = pack_blob(by_lod[0])

        def tile_grid(feats, tile_deg):
            w = max(lon1 - lon0, 1e-6)
            h = max(lat1 - lat0, 1e-6)
            nx = max(1, int(math.ceil(w / tile_deg)))
            ny = max(1, int(math.ceil(h / tile_deg)))
            tw = w / nx
            th = h / ny
            # overlap so roads are not hairline-gapped at tile edges
            pad = min(tw, th) * 0.04
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
                                buckets[ty][tx].append(Feat(f.layer, f.flags, f.min_scale, pts))
                        else:
                            for piece in clip_linestring(f.coords, box):
                                if len(piece) >= 2:
                                    buckets[ty][tx].append(
                                        Feat(f.layer, f.flags, f.min_scale, piece)
                                    )
            tiles = []
            for ty in range(ny):
                for tx in range(nx):
                    blob, npts, nfeats = pack_blob(buckets[ty][tx], max_pts=65535)
                    tiles.append((blob, npts, nfeats))
            return nx, ny, tw, th, tiles

        lod1 = tile_grid(by_lod[1], tile_deg_lod1)
        lod2 = tile_grid(by_lod[2], tile_deg_lod2)

        labels = self.labels
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
                    0,
                    0,
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
            out.write(ov_blob)
            for blob, npts, nfeats in lod1[4] + lod2[4]:
                if npts and nfeats:
                    out.write(blob)

        kb = cur / 1024.0
        print(
            f"Wrote {path}: MDV2 overview {ov_pts} pts / {ov_feats} feats, "
            f"{len(labels)} labels, LOD1 {lod1[0]}x{lod1[1]} @{tile_deg_lod1}deg, "
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

    for _wid, w in ways.items():
        tags = w.get("tags") or {}
        coords = way_coords(w)
        if len(coords) < 2:
            continue

        if "highway" in tags:
            cls = classify_highway(tags)
            if cls:
                layer, ms, fl = cls
                b.add_line(coords, layer, ms, closed=False, extra_flags=fl)
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
        cls = classify_highway(props)
        extra = 0
        if cls:
            layer, ms, extra = cls
        if gtype == "LineString":
            b.add_line(coords, layer, ms, closed=False, extra_flags=extra)
        elif gtype == "MultiLineString":
            for line in coords:
                b.add_line(line, layer, ms, closed=False, extra_flags=extra)
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
            cls = classify_highway(tags)
            if cls:
                layer, ms, fl = cls
                b.add_line(coords, layer, ms, closed=False, extra_flags=fl)
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
                print(f"  ... {self.ways_done} ways, {len(b.raw)} feats")

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
    )
    print("Done. Copy to SD: /meshdeck-maps/  then open NewMaps on the device.")
    print("Attribution: (c) OpenStreetMap contributors")


if __name__ == "__main__":
    main()
