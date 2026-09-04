#!/usr/bin/env python3
"""Write a tiny synthetic CDA-area .mdv so NewMaps works before a full OSM build."""
import math
import os
import struct

MAGIC = 0x3156444D
scale = 100000.0
lat0, lat1, lon0, lon1 = 47.6, 47.75, -116.9, -116.6
pts = []
feats = []

def add_line(coords, layer, min_scale, closed=False):
    start = len(pts)
    for lon, lat in coords:
        pts.append((int(round(lat * scale)), int(round(lon * scale))))
    flags = 1 if closed else 0
    feats.append((layer, flags, min_scale, start, len(coords)))

# lake-ish polygon
ring = [
    (-116.80, 47.68),
    (-116.78, 47.70),
    (-116.75, 47.69),
    (-116.76, 47.67),
    (-116.80, 47.68),
]
add_line(ring, 0, 16, closed=True)

# curved primary road
road = []
for i in range(48):
    t = i / 47
    lon = -116.85 + t * 0.22
    lat = 47.65 + 0.035 * math.sin(t * math.pi * 2) + t * 0.06
    road.append((lon, lat))
add_line(road, 12, 24)

# residential
road2 = []
for i in range(36):
    t = i / 35
    lon = -116.82 + t * 0.1
    lat = 47.67 + 0.015 * math.sin(t * 8)
    road2.append((lon, lat))
add_line(road2, 9, 96)

# park
park = [
    (-116.79, 47.66),
    (-116.77, 47.66),
    (-116.77, 47.675),
    (-116.79, 47.675),
    (-116.79, 47.66),
]
add_line(park, 1, 48, closed=True)

labels = [
    (47.677, -116.78, 0, 16, "Coeur d Alene"),
    (47.70, -116.80, 1, 32, "Hayden"),
]

out = os.path.join(
    os.path.dirname(__file__), "..", "sdcard", "meshdeck-maps", "demo-cda.mdv"
)
out = os.path.normpath(out)
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "wb") as f:
    f.write(
        struct.pack(
            "<IHH4ffIII",
            MAGIC,
            1,
            0,
            lat0,
            lat1,
            lon0,
            lon1,
            scale,
            len(pts),
            len(feats),
            len(labels),
        )
    )
    for a, b in pts:
        f.write(struct.pack("<ii", a, b))
    for fe in feats:
        f.write(struct.pack("<BBHII", *fe))
    for lat, lon, kind, ms, name in labels:
        nb = name.encode("ascii")[:19]
        nb = nb + b"\0" * (20 - len(nb))
        f.write(
            struct.pack(
                "<iiBB20s",
                int(round(lat * scale)),
                int(round(lon * scale)),
                kind,
                ms // 4,
                nb,
            )
        )
print(f"Wrote {out} ({os.path.getsize(out)} bytes, {len(pts)} pts)")
