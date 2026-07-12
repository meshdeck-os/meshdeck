#!/usr/bin/env python3
"""Generate a MeshDeck SD map pack (.mdm) from Natural Earth GeoJSON.

A map pack adds high-detail coastline + extra town labels for a region of your
choice. Copy the .mdm file(s) into a folder called  meshdeck-maps  on a FAT32
SD card; MeshDeck loads every pack it finds at boot (up to 4).

Usage:
  gen_sdmap.py <ne_repo_dir> <out.mdm> <lon_min> <lat_min> <lon_max> <lat_max> [tolerance_deg]

Example (UK + Ireland at full 10m detail):
  gen_sdmap.py natural-earth-geojson uk.mdm -11 49.5 2.2 61.2 0.004

Data source repo: https://github.com/martynafford/natural-earth-geojson
(needs 10m/physical/ne_10m_coastline.json and 50m/cultural/ne_50m_populated_places_simple.json)
"""
import json
import struct
import sys

MAGIC = 0x314D444D   # "MDM1"


def load_lines(path):
    with open(path) as f:
        gj = json.load(f)
    lines = []
    for feat in gj["features"]:
        g = feat["geometry"]
        if g["type"] == "LineString":
            lines.append(g["coordinates"])
        elif g["type"] == "MultiLineString":
            lines.extend(g["coordinates"])
    return lines


def simplify(coords, tol):
    if len(coords) <= 2 or tol <= 0:
        return coords
    out = [coords[0]]
    for p in coords[1:-1]:
        q = out[-1]
        if abs(p[0] - q[0]) + abs(p[1] - q[1]) >= tol:
            out.append(p)
    out.append(coords[-1])
    return out


def clip_to_box(coords, box):
    lon0, lat0, lon1, lat1 = box
    segs, cur = [], []
    for lon, lat in coords:
        if lon0 <= lon <= lon1 and lat0 <= lat <= lat1:
            cur.append((lon, lat))
        else:
            if len(cur) > 1:
                segs.append(cur)
            cur = []
    if len(cur) > 1:
        segs.append(cur)
    return segs


def main():
    repo, outfile = sys.argv[1], sys.argv[2]
    lon0, lat0, lon1, lat1 = map(float, sys.argv[3:7])
    tol = float(sys.argv[7]) if len(sys.argv) > 7 else 0.004
    box = (lon0, lat0, lon1, lat1)

    # pick the largest int16-safe scale for this bbox (max |deg| * scale < 32700)
    m = max(abs(lon0), abs(lat0), abs(lon1), abs(lat1))
    scale = int(32700 / m) if m > 0 else 400
    scale = min(scale, 500)

    raw = load_lines(f"{repo}/10m/physical/ne_10m_coastline.json")
    segs = []
    for ln in raw:
        segs.extend(clip_to_box(ln, box))

    pts, idx = [], []
    for s in segs:
        s = simplify(s, tol)
        if len(s) < 2:
            continue
        start = len(pts)
        for lon, lat in s:
            pts.append((int(round(lat * scale)), int(round(lon * scale))))
        idx.append((start, len(s)))

    with open(f"{repo}/50m/cultural/ne_50m_populated_places_simple.json") as f:
        places = json.load(f)["features"]
    cities = []
    for p in places:
        lon, lat = p["geometry"]["coordinates"]
        if not (lon0 <= lon <= lon1 and lat0 <= lat <= lat1):
            continue
        pr = p["properties"]
        name = pr.get("name", "?")
        if any(ord(ch) > 126 for ch in str(name)):
            name = pr.get("nameascii", "?")
            if any(ord(ch) > 126 for ch in str(name)):
                continue
        sr = pr.get("scalerank", 6)
        rank = 0 if sr <= 0 else 1 if sr <= 1 else 2 if sr <= 3 else 3 if sr <= 6 else 4
        cities.append((str(name)[:14], lat, lon, rank))

    with open(outfile, "wb") as out:
        out.write(struct.pack("<If4hIII", MAGIC, float(scale),
                              int(lat0 * 100), int(lat1 * 100),
                              int(lon0 * 100), int(lon1 * 100),
                              len(pts), len(idx), len(cities)))
        for lat, lon in pts:
            out.write(struct.pack("<hh", lat, lon))
        for start, count in idx:
            out.write(struct.pack("<IH", start, count))
        for name, lat, lon, rank in cities:
            out.write(struct.pack("<hhB15s", int(round(lat * 100)), int(round(lon * 100)),
                                  rank, name.encode()[:15]))

    kb = (len(pts) * 4 + len(idx) * 6 + len(cities) * 20 + 28) / 1024
    print(f"{outfile}: {len(pts)} points, {len(idx)} lines, {len(cities)} places, {kb:.0f} KB, scale {scale}")


if __name__ == "__main__":
    main()
