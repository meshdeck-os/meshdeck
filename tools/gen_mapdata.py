#!/usr/bin/env python3
"""Generate ui/mapdata.h for MeshDeck from Natural Earth GeoJSON.

Inputs (from https://github.com/martynafford/natural-earth-geojson):
  110m/physical/ne_110m_coastline.json          -> world outline
  50m/physical/ne_50m_coastline.json            -> Europe detail (clipped)
  110m/cultural/ne_110m_populated_places_simple.json -> world cities

Usage: gen_mapdata.py <ne_repo_dir> <out_file>
"""
import json
import sys

EU = (-25.0, 34.0, 45.0, 72.0)   # lon_min, lat_min, lon_max, lat_max


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
    """radial distance simplification"""
    if len(coords) <= 2:
        return coords
    out = [coords[0]]
    for p in coords[1:-1]:
        q = out[-1]
        if abs(p[0] - q[0]) + abs(p[1] - q[1]) >= tol:
            out.append(p)
    out.append(coords[-1])
    return out


def clip_to_box(coords, box):
    """split a line into segments fully inside the box (coarse)"""
    lon0, lat0, lon1, lat1 = box
    segs = []
    cur = []
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


def emit_dataset(out, name, lines, scale, tol):
    pts = []
    idx = []
    for ln in lines:
        ln = simplify(ln, tol)
        if len(ln) < 2:
            continue
        start = len(pts)
        for lon, lat in ln:
            pts.append((int(round(lat * scale)), int(round(lon * scale))))
        idx.append((start, len(ln)))
    out.write(f"static const MapPt {name}_PTS[{len(pts)}] PROGMEM = {{\n")
    for i in range(0, len(pts), 8):
        row = ",".join(f"{{{a},{b}}}" for a, b in pts[i:i + 8])
        out.write("  " + row + ",\n")
    out.write("};\n")
    out.write(f"static const MapLine {name}_LINES[{len(idx)}] PROGMEM = {{\n")
    for i in range(0, len(idx), 10):
        row = ",".join(f"{{{a},{b}}}" for a, b in idx[i:i + 10])
        out.write("  " + row + ",\n")
    out.write("};\n")
    out.write(f"#define {name}_NLINES {len(idx)}\n")
    out.write(f"#define {name}_SCALE {scale}\n\n")
    return len(pts)


UK_CITIES = [
    # name, lat, lon, rank
    ("London", 51.507, -0.128, 0), ("Birmingham", 52.480, -1.903, 1),
    ("Manchester", 53.481, -2.243, 1), ("Glasgow", 55.864, -4.252, 1),
    ("Leeds", 53.800, -1.549, 2), ("Liverpool", 53.408, -2.992, 2),
    ("Newcastle", 54.978, -1.618, 2), ("Sheffield", 53.381, -1.470, 2),
    ("Bristol", 51.455, -2.587, 2), ("Nottingham", 52.954, -1.158, 2),
    ("Edinburgh", 55.953, -3.188, 1), ("Cardiff", 51.481, -3.179, 2),
    ("Belfast", 54.597, -5.930, 2), ("Leicester", 52.637, -1.140, 3),
    ("Coventry", 52.407, -1.520, 3), ("Hull", 53.746, -0.336, 3),
    ("Bradford", 53.796, -1.759, 3), ("Stoke", 53.003, -2.179, 3),
    ("Southampton", 50.910, -1.404, 3), ("Portsmouth", 50.816, -1.084, 3),
    ("Plymouth", 50.376, -4.143, 3), ("Norwich", 52.628, 1.299, 3),
    ("Aberdeen", 57.150, -2.094, 3), ("Dundee", 56.462, -2.970, 3),
    ("Inverness", 57.478, -4.224, 4), ("Exeter", 50.718, -3.534, 4),
    ("Cambridge", 52.205, 0.119, 4), ("Oxford", 51.752, -1.258, 4),
    ("Brighton", 50.823, -0.138, 4), ("York", 53.960, -1.081, 4),
    ("Carlisle", 54.893, -2.933, 4), ("Swansea", 51.621, -3.943, 4),
    ("Derry", 54.997, -7.309, 4), ("Dublin", 53.349, -6.260, 1),
    ("Cork", 51.898, -8.476, 3), ("Galway", 53.271, -9.049, 4),
    ("Limerick", 52.668, -8.630, 4), ("Penzance", 50.119, -5.537, 5),
    ("Dover", 51.128, 1.313, 5), ("Holyhead", 53.309, -4.633, 5),
    ("Fort William", 56.820, -5.105, 5), ("Stornoway", 58.209, -6.387, 5),
    ("Lerwick", 60.155, -1.145, 5), ("Kirkwall", 58.981, -2.960, 5),
]


def main():
    repo, outfile = sys.argv[1], sys.argv[2]

    world = load_lines(f"{repo}/110m/physical/ne_110m_coastline.json")
    eu50 = load_lines(f"{repo}/50m/physical/ne_50m_coastline.json")
    eu_lines = []
    for ln in eu50:
        eu_lines.extend(clip_to_box(ln, EU))

    with open(f"{repo}/110m/cultural/ne_110m_populated_places_simple.json") as f:
        places = json.load(f)["features"]

    cities = []
    for p in places:
        pr = p["properties"]
        lon, lat = p["geometry"]["coordinates"]
        name = pr.get("name", "?")
        if any(ord(ch) > 126 for ch in name):
            name = pr.get("nameascii", name)
            if any(ord(ch) > 126 for ch in str(name)):
                continue
        rank = pr.get("scalerank", 4)
        rank = 0 if rank <= 0 else (1 if rank <= 1 else (2 if rank <= 3 else (3 if rank <= 6 else 4)))
        cities.append((str(name)[:13], lat, lon, rank))

    # add UK/IE detail cities, avoiding duplicates
    have = {c[0] for c in cities}
    for name, lat, lon, rank in UK_CITIES:
        if name not in have:
            cities.append((name[:13], lat, lon, rank))

    with open(outfile, "w") as out:
        out.write("// Auto-generated by tools/gen_mapdata.py - do not edit\n")
        out.write("// Data: Natural Earth (public domain), naturalearthdata.com\n")
        out.write("#pragma once\n#include <Arduino.h>\n\n")
        out.write("struct MapPt { int16_t lat, lon; };\n")
        out.write("struct MapLine { uint16_t start; uint16_t count; };\n")
        out.write("struct MapCity { int16_t lat100, lon100; uint8_t rank; char name[14]; };\n\n")

        n1 = emit_dataset(out, "WORLD", world, 100, 0.0)
        n2 = emit_dataset(out, "EU", eu_lines, 200, 0.012)

        out.write(f"static const MapCity CITIES[{len(cities)}] PROGMEM = {{\n")
        for name, lat, lon, rank in cities:
            esc = name.replace("\\", "").replace('"', "")
            out.write(f'  {{{int(round(lat * 100))},{int(round(lon * 100))},{rank},"{esc}"}},\n')
        out.write("};\n")
        out.write(f"#define N_CITIES {len(cities)}\n")
        # Europe detail window
        out.write(f"#define EU_LON_MIN {EU[0]}\n#define EU_LAT_MIN {EU[1]}\n")
        out.write(f"#define EU_LON_MAX {EU[2]}\n#define EU_LAT_MAX {EU[3]}\n")

    print(f"world pts: {n1}, eu pts: {n2}, cities: {len(cities)}")


if __name__ == "__main__":
    main()
