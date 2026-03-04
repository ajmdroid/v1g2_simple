#!/usr/bin/env python3
"""
build_road_map.py — Generate road_map.bin for ESP32 lockout road-snapping.

Reads US road geometry (motorway / trunk / primary) from an OSM PBF file,
applies Ramer-Douglas-Peucker simplification, and writes a compact binary
spatial-index file for the ESP32 to read from SD card.

Requires: pip install osmium

Usage:
    # Download a PBF from Geofabrik, then convert:
    python scripts/build_road_map.py --download              # Auto-download US PBF
    python scripts/build_road_map.py --pbf us-latest.osm.pbf # From local PBF
    python scripts/build_road_map.py --pbf colorado-latest.osm.pbf --region test

    # Options:
    python scripts/build_road_map.py --pbf us.pbf --tolerance 200
    python scripts/build_road_map.py --pbf us.pbf --classes motorway,trunk

    # Legacy (Overpass API — slow, unreliable):
    python scripts/build_road_map.py --overpass
    python scripts/build_road_map.py --overpass --region test

Output: road_map.bin (copy to ESP32 SD card root)

Binary format: see docs/ROAD_MAP_FORMAT.md
"""

import argparse
import json
import math
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# ═══════════════════════════════════════════════════════════════════════════════
# Binary format constants
# ═══════════════════════════════════════════════════════════════════════════════

MAGIC = b"RMAP"
FORMAT_VERSION = 1
HEADER_SIZE = 64

# Road class enum — must match ESP32 reader
RC_MOTORWAY = 0
RC_TRUNK = 1
RC_PRIMARY = 2

HIGHWAY_TO_CLASS = {
    "motorway": RC_MOTORWAY, "motorway_link": RC_MOTORWAY,
    "trunk": RC_TRUNK, "trunk_link": RC_TRUNK,
    "primary": RC_PRIMARY, "primary_link": RC_PRIMARY,
}

# ═══════════════════════════════════════════════════════════════════════════════
# Geographic constants
# ═══════════════════════════════════════════════════════════════════════════════

DEFAULT_CELL_DEG = 0.5          # Grid cell size in degrees
DEFAULT_TOLERANCE_M = 100       # RDP simplification tolerance (metres)

# US 48 states bounding box (south, west, north, east)
US48_BBOX = (24.0, -126.0, 50.0, -66.0)

# Small test region (I-25/I-70 interchange, Denver) — quick download
TEST_BBOX = (39.72, -105.02, 39.80, -104.92)

# Geofabrik PBF downloads
GEOFABRIK_US_URL = "https://download.geofabrik.de/north-america/us-latest.osm.pbf"
GEOFABRIK_CO_URL = "https://download.geofabrik.de/north-america/us/colorado-latest.osm.pbf"

# Overpass API (legacy fallback)
OVERPASS_URL = "https://overpass-api.de/api/interpreter"
OVERPASS_TIMEOUT = 180  # seconds
CHUNK_PAUSE_S = 5       # polite pause between queries
MIN_CHUNK_DEG = 0.5     # stop subdividing below this


# ═══════════════════════════════════════════════════════════════════════════════
# Geometry helpers
# ═══════════════════════════════════════════════════════════════════════════════

def to_e5(deg):
    """Degrees → E5 fixed-point (1° = 100,000)."""
    return int(round(deg * 100_000))


def _perp_dist_m(plat, plon, alat, alon, blat, blon, cos_lat):
    """Perpendicular distance in metres from point P to segment A→B.

    Uses flat-Earth approximation scaled by cos(reference latitude).
    Good enough for RDP at ≤500 m tolerance in the continental US.
    """
    M = 111_320.0
    Mlon = M * cos_lat

    # Project to local metres relative to A
    px, py = (plat - alat) * M, (plon - alon) * Mlon
    bx, by = (blat - alat) * M, (blon - alon) * Mlon

    d2 = bx * bx + by * by
    if d2 < 1e-10:
        return math.hypot(px, py)

    t = max(0.0, min(1.0, (px * bx + py * by) / d2))
    return math.hypot(px - t * bx, py - t * by)


def rdp_simplify(coords, tolerance_m):
    """Iterative Ramer-Douglas-Peucker simplification.

    coords: list of (lat, lon) in degrees.
    tolerance_m: tolerance in metres.
    Returns a new list with only the kept points.
    """
    n = len(coords)
    if n <= 2:
        return list(coords)

    ref_lat = (coords[0][0] + coords[-1][0]) / 2.0
    cos_lat = math.cos(math.radians(ref_lat))

    keep = [False] * n
    keep[0] = keep[-1] = True

    stack = [(0, n - 1)]
    while stack:
        lo, hi = stack.pop()
        if hi - lo < 2:
            continue
        alat, alon = coords[lo]
        blat, blon = coords[hi]
        best_d, best_i = 0.0, lo
        for i in range(lo + 1, hi):
            d = _perp_dist_m(
                coords[i][0], coords[i][1],
                alat, alon, blat, blon, cos_lat,
            )
            if d > best_d:
                best_d, best_i = d, i
        if best_d > tolerance_m:
            keep[best_i] = True
            stack.append((lo, best_i))
            stack.append((best_i, hi))

    return [c for c, k in zip(coords, keep) if k]


# ═══════════════════════════════════════════════════════════════════════════════
# Overpass API
# ═══════════════════════════════════════════════════════════════════════════════

def _make_chunks(bbox, lat_step=2.0, lon_step=5.0):
    """Split bounding box into Overpass query chunks."""
    south, west, north, east = bbox
    chunks = []
    lat = south
    while lat < north:
        lon = west
        while lon < east:
            chunks.append((
                lat, lon,
                min(lat + lat_step, north),
                min(lon + lon_step, east),
            ))
            lon += lon_step
        lat += lat_step
    return chunks


def _fetch_chunk_single(bbox, cache_dir, timeout=OVERPASS_TIMEOUT, retries=3):
    """Fetch road data for one geographic chunk (cached on disk).

    Returns (result_dict, success_bool).
    """
    s, w, n, e = bbox
    tag = f"{s:.2f}_{w:.2f}_{n:.2f}_{e:.2f}"
    cache_path = Path(cache_dir) / f"chunk_{tag}.json"

    # Return cached result if available
    if cache_path.exists():
        sz = cache_path.stat().st_size
        print(f"    cached  ({sz:>10,} B)  {cache_path.name}")
        with open(cache_path, "r") as f:
            return json.load(f), True

    query = (
        f'[out:json][timeout:{timeout}];'
        f'(way["highway"~"^(motorway|motorway_link|trunk|trunk_link'
        f'|primary|primary_link)$"]'
        f'({s},{w},{n},{e}););out geom;'
    )
    body = urllib.parse.urlencode({"data": query}).encode("utf-8")

    for attempt in range(retries):
        try:
            label = f"({s:.1f},{w:.1f})→({n:.1f},{e:.1f})"
            print(f"    fetch   {label} ...", end="", flush=True)
            req = urllib.request.Request(OVERPASS_URL, data=body)
            req.add_header("User-Agent", "V1Simple-RoadMapBuilder/1.0")
            with urllib.request.urlopen(req, timeout=timeout + 60) as resp:
                raw = resp.read()
            result = json.loads(raw.decode("utf-8"))
            cache_path.parent.mkdir(parents=True, exist_ok=True)
            with open(cache_path, "w") as f:
                json.dump(result, f)
            elems = len(result.get("elements", []))
            print(f"  {elems:,} ways  ({len(raw):,} B)")
            return result, True
        except Exception as ex:
            wait = (attempt + 1) * 30
            print(f"\n    ERROR: {ex}  (retry {attempt+1}/{retries} in {wait}s)")
            time.sleep(wait)

    return {"elements": []}, False


def _fetch_chunk_adaptive(bbox, cache_dir, depth=0, timeout=OVERPASS_TIMEOUT):
    """Fetch a chunk, auto-splitting into 4 sub-chunks on failure.

    Returns a list of result dicts (one per successful sub-fetch).
    """
    result, ok = _fetch_chunk_single(bbox, cache_dir, timeout=timeout)
    if ok:
        return [result]

    # Check if further splitting is possible
    s, w, n, e = bbox
    lat_span = n - s
    lon_span = e - w
    if lat_span <= MIN_CHUNK_DEG and lon_span <= MIN_CHUNK_DEG:
        print(f"    FAILED chunk (cannot split further)", file=sys.stderr)
        return []

    # Split into 4 quadrants
    mid_lat = (s + n) / 2.0
    mid_lon = (w + e) / 2.0
    sub = [
        (s, w, mid_lat, mid_lon),
        (s, mid_lon, mid_lat, e),
        (mid_lat, w, n, mid_lon),
        (mid_lat, mid_lon, n, e),
    ]
    prefix = "  " * (depth + 1)
    print(f"{prefix}⤷ splitting into 4 sub-chunks")
    results = []
    for i, sub_bbox in enumerate(sub):
        time.sleep(CHUNK_PAUSE_S)
        print(f"{prefix}  sub [{i+1}/4]", end="")
        results.extend(
            _fetch_chunk_adaptive(sub_bbox, cache_dir, depth + 1, timeout)
        )
    return results


# ═══════════════════════════════════════════════════════════════════════════════
# PBF loading (primary path — fast, reliable, offline)
# ═══════════════════════════════════════════════════════════════════════════════

def _download_pbf(url, dest_path):
    """Download a PBF file from Geofabrik with progress display."""
    dest = Path(dest_path)
    if dest.exists():
        sz = dest.stat().st_size
        print(f"  PBF already downloaded: {dest} ({sz / 1024 / 1024:.0f} MB)")
        return str(dest)

    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  Downloading {url}")
    print(f"  → {dest}")

    req = urllib.request.Request(url)
    req.add_header("User-Agent", "V1Simple-RoadMapBuilder/1.0")

    try:
        with urllib.request.urlopen(req, timeout=600) as resp:
            total = int(resp.headers.get("Content-Length", 0))
            downloaded = 0
            chunk_size = 1024 * 1024  # 1 MB chunks
            with open(str(dest), "wb") as f:
                while True:
                    chunk = resp.read(chunk_size)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if total > 0:
                        pct = downloaded * 100 // total
                        mb = downloaded / 1024 / 1024
                        total_mb = total / 1024 / 1024
                        print(f"\r  {mb:.0f}/{total_mb:.0f} MB ({pct}%)",
                              end="", flush=True)
                    else:
                        mb = downloaded / 1024 / 1024
                        print(f"\r  {mb:.0f} MB", end="", flush=True)
        print()  # newline after progress
    except Exception as ex:
        # Clean up partial download
        if dest.exists():
            dest.unlink()
        print(f"\n  ERROR downloading PBF: {ex}", file=sys.stderr)
        sys.exit(1)

    sz = dest.stat().st_size
    print(f"  Downloaded {sz / 1024 / 1024:.0f} MB")
    return str(dest)


def _load_from_pbf(pbf_path, allowed_classes, bbox=None):
    """Load road segments from an OSM PBF file using pyosmium.

    For large PBFs (>1 GB), uses osmium-tool to pre-filter highways
    into a small intermediate PBF first (fast, low memory). Falls back
    to a disk-backed node index if osmium-tool is not installed.

    Args:
        pbf_path: Path to .osm.pbf file.
        allowed_classes: Set of road class enums (RC_MOTORWAY, etc.)
        bbox: Optional (south, west, north, east) filter. Ways with ALL
              points outside the bbox are dropped.

    Returns list of Segment.
    """
    try:
        import osmium
    except ImportError:
        print("ERROR: pyosmium not installed.\n"
              "  Install with:  pip install osmium\n"
              "  Or:            pip install -r requirements.txt",
              file=sys.stderr)
        sys.exit(1)

    import shutil
    import subprocess
    import tempfile

    class RoadHandler(osmium.SimpleHandler):
        def __init__(self, allowed, bbox_filter):
            super().__init__()
            self.allowed = allowed
            self.bbox_filter = bbox_filter
            self.segments = []
            self.way_count = 0

        def way(self, w):
            self.way_count += 1
            if self.way_count % 500_000 == 0:
                print(f"\r  Scanned {self.way_count:,} ways, "
                      f"found {len(self.segments):,} roads ...",
                      end="", flush=True)

            hw = w.tags.get("highway", "")
            rc = HIGHWAY_TO_CLASS.get(hw)
            if rc is None or rc not in self.allowed:
                return

            try:
                pts = [(n.lat, n.lon) for n in w.nodes]
            except Exception:
                return  # Unresolvable node locations

            if len(pts) < 2:
                return

            # Bbox filter: keep if ANY point is inside the bbox.
            if self.bbox_filter:
                s, west, n, east = self.bbox_filter
                if not any(s <= lat <= n and west <= lon <= east
                           for lat, lon in pts):
                    return

            oneway = w.tags.get("oneway") == "yes"
            self.segments.append(Segment(rc, oneway, pts))

    # For large files, pre-filter with osmium-tool to avoid OOM.
    # osmium tags-filter extracts just highway ways + their nodes,
    # reducing a 10 GB US PBF to ~200-400 MB.
    file_size_mb = os.path.getsize(pbf_path) / (1024 * 1024)
    actual_pbf = pbf_path

    if file_size_mb > 1000 and shutil.which("osmium"):
        print(f"  Pre-filtering {file_size_mb:.0f} MB PBF with osmium-tool ...")
        hw_filter = ("w/highway=motorway,motorway_link,"
                     "trunk,trunk_link,primary,primary_link")
        filtered_path = pbf_path + ".highways.osm.pbf"
        if os.path.exists(filtered_path):
            sz = os.path.getsize(filtered_path) / (1024 * 1024)
            print(f"  Pre-filtered PBF already exists: {filtered_path} "
                  f"({sz:.0f} MB)")
        else:
            t0 = time.monotonic()
            result = subprocess.run(
                ["osmium", "tags-filter", pbf_path, hw_filter,
                 "-o", filtered_path, "--overwrite"],
                capture_output=True, text=True)
            dt = time.monotonic() - t0
            if result.returncode == 0:
                sz = os.path.getsize(filtered_path) / (1024 * 1024)
                print(f"  Filtered to {sz:.0f} MB in {dt:.1f}s")
            else:
                print(f"  osmium-tool failed: {result.stderr.strip()}")
                print(f"  Falling back to disk-backed index ...")
                filtered_path = None
        if filtered_path and os.path.exists(filtered_path):
            actual_pbf = filtered_path

    handler = RoadHandler(allowed_classes, bbox)
    print(f"  Reading {actual_pbf} ...")
    t0 = time.monotonic()

    # Use disk-backed index for large files to avoid OOM.
    if file_size_mb > 1000 and actual_pbf == pbf_path:
        # No pre-filter available — use sparse_file_array index.
        print(f"  Using disk-backed node index (file is {file_size_mb:.0f} MB)")
        print(f"  TIP: install osmium-tool for 10× faster processing:")
        print(f"       brew install osmium-tool")
        handler.apply_file(actual_pbf, locations=True,
                           idx="sparse_file_array")
    else:
        handler.apply_file(actual_pbf, locations=True)

    dt = time.monotonic() - t0
    print(f"\r  Scanned {handler.way_count:,} ways in {dt:.1f}s — "
          f"found {len(handler.segments):,} road segments")
    return handler.segments


# ═══════════════════════════════════════════════════════════════════════════════
# Road segment data structure
# ═══════════════════════════════════════════════════════════════════════════════

class Segment:
    """A single road segment after simplification."""
    __slots__ = ("rc", "oneway", "pts")

    def __init__(self, rc, oneway, pts):
        self.rc = rc            # road class enum (0/1/2)
        self.oneway = oneway    # bool
        self.pts = pts          # list of (lat_deg, lon_deg)


# ═══════════════════════════════════════════════════════════════════════════════
# Parsing
# ═══════════════════════════════════════════════════════════════════════════════

def _parse_elements(elements, seen_ids, allowed_classes):
    """Parse Overpass JSON elements → list of Segment."""
    out = []
    for el in elements:
        if el.get("type") != "way":
            continue
        wid = el["id"]
        if wid in seen_ids:
            continue
        seen_ids.add(wid)
        tags = el.get("tags", {})
        hw = tags.get("highway", "")
        rc = HIGHWAY_TO_CLASS.get(hw)
        if rc is None or rc not in allowed_classes:
            continue
        geom = el.get("geometry", [])
        if len(geom) < 2:
            continue
        pts = [(g["lat"], g["lon"]) for g in geom]
        out.append(Segment(rc, tags.get("oneway") == "yes", pts))
    return out


# ═══════════════════════════════════════════════════════════════════════════════
# Way merging — join adjacent OSM ways into longer polylines
# ═══════════════════════════════════════════════════════════════════════════════

def _merge_ways(segments):
    """Merge adjacent segments that share endpoints and road class/oneway.

    OSM splits highways at every intersection and tag change, creating
    millions of tiny 2-point segments. Merging connected ways with the
    same properties into continuous polylines dramatically reduces segment
    count and binary file size (fewer 12-byte headers).

    At real intersections (3+ ways meeting at a point), segments are NOT
    merged — those are natural break points.

    Returns (merged_segments, merge_count).
    """
    if not segments:
        return segments, 0

    # Round coords to ~1m precision to handle floating-point mismatches.
    def snap_key(lat, lon):
        return (round(lat, 5), round(lon, 5))

    # Build endpoint → segment index mapping.
    # Key = (rc, oneway, snapped_coord), Value = list of segment indices.
    from collections import defaultdict
    endpoint_map = defaultdict(list)

    for i, seg in enumerate(segments):
        key_props = (seg.rc, seg.oneway)
        start = snap_key(*seg.pts[0])
        end = snap_key(*seg.pts[-1])
        endpoint_map[(*key_props, start)].append(i)
        endpoint_map[(*key_props, end)].append(i)

    # Find mergeable pairs: endpoints where exactly 2 segments meet
    # (same road class and oneway flag).
    merged = [False] * len(segments)
    merge_count = 0

    # Use Union-Find to chain multi-segment merges efficiently.
    parent = list(range(len(segments)))
    chains = {i: list(seg.pts) for i, seg in enumerate(segments)}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for key, indices in endpoint_map.items():
        if len(indices) != 2:
            continue  # Intersection (3+) or dead end (1) — don't merge.

        a_idx, b_idx = indices
        a_root = find(a_idx)
        b_root = find(b_idx)
        if a_root == b_root:
            continue  # Already in the same chain (loop).

        # Determine which ends connect and concatenate.
        a_pts = chains[a_root]
        b_pts = chains[b_root]
        a_start = snap_key(*a_pts[0])
        a_end = snap_key(*a_pts[-1])
        b_start = snap_key(*b_pts[0])
        b_end = snap_key(*b_pts[-1])

        shared_coord = key[2]  # The coordinate from the endpoint key.

        if a_end == shared_coord and b_start == shared_coord:
            # A-end connects to B-start: A + B[1:]
            new_pts = a_pts + b_pts[1:]
        elif a_end == shared_coord and b_end == shared_coord:
            # A-end connects to B-end: A + reversed(B)[1:]
            new_pts = a_pts + b_pts[-2::-1]
        elif a_start == shared_coord and b_start == shared_coord:
            # A-start connects to B-start: reversed(A) + B[1:]
            new_pts = a_pts[::-1] + b_pts[1:]
        elif a_start == shared_coord and b_end == shared_coord:
            # A-start connects to B-end: B + A[1:]
            new_pts = b_pts + a_pts[1:]
        else:
            continue  # Shouldn't happen, but be safe.

        # Merge: b_root → a_root
        parent[b_root] = a_root
        chains[a_root] = new_pts
        del chains[b_root]
        merge_count += 1

    # Collect results.
    result = []
    for root, pts in chains.items():
        seg = segments[root]
        result.append(Segment(seg.rc, seg.oneway, pts))

    return result, merge_count


# ═══════════════════════════════════════════════════════════════════════════════
# Processing pipeline
# ═══════════════════════════════════════════════════════════════════════════════

def _seg_length_m(pts):
    """Approximate total length of a polyline in metres (flat-earth)."""
    total = 0.0
    for i in range(len(pts) - 1):
        dlat = (pts[i + 1][0] - pts[i][0]) * 111_320.0
        cos_lat = math.cos(math.radians(pts[i][0]))
        dlon = (pts[i + 1][1] - pts[i][1]) * 111_320.0 * cos_lat
        total += math.sqrt(dlat * dlat + dlon * dlon)
    return total


def _drop_short(segments, min_length_m):
    """Drop segments shorter than min_length_m.

    Short segments are typically ramps, slip roads, and interchange
    connectors that aren't useful for lockout road-snapping.
    """
    if min_length_m <= 0:
        return segments, 0
    result = []
    dropped = 0
    for seg in segments:
        if _seg_length_m(seg.pts) >= min_length_m:
            result.append(seg)
        else:
            dropped += 1
    return result, dropped


def _simplify_all(segments, tolerance_m):
    """Apply RDP to every segment; drop any that collapse to < 2 points."""
    result = []
    dropped = 0
    for seg in segments:
        simplified = rdp_simplify(seg.pts, tolerance_m)
        if len(simplified) >= 2:
            seg.pts = simplified
            result.append(seg)
        else:
            dropped += 1
    return result, dropped


def _split_long(segments, max_extent_deg):
    """Split segments whose bounding box exceeds max_extent_deg.

    Ensures every segment fits within ~1 grid cell of its midpoint,
    so the ESP32's 3×3 cell search is guaranteed to find it.
    Also ensures no consecutive-point delta exceeds the int16 range
    (±32767 E5 ≈ ±36.4 km) used by the binary encoding.
    """
    DELTA_LIMIT_DEG = 32767.0 / 100_000.0  # ≈ 0.32767°

    out = []
    splits = 0
    stack = list(segments)
    while stack:
        seg = stack.pop()
        lats = [p[0] for p in seg.pts]
        lons = [p[1] for p in seg.pts]
        extent = max(max(lats) - min(lats), max(lons) - min(lons))

        if len(seg.pts) <= 2:
            # Check if the 2-point segment exceeds the int16 delta limit.
            if len(seg.pts) == 2:
                dlat = abs(seg.pts[1][0] - seg.pts[0][0])
                dlon = abs(seg.pts[1][1] - seg.pts[0][1])
                if dlat > DELTA_LIMIT_DEG or dlon > DELTA_LIMIT_DEG:
                    # Insert midpoint to split into two safe sub-segments.
                    mid = ((seg.pts[0][0] + seg.pts[1][0]) / 2.0,
                           (seg.pts[0][1] + seg.pts[1][1]) / 2.0)
                    stack.append(Segment(seg.rc, seg.oneway,
                                         [seg.pts[0], mid]))
                    stack.append(Segment(seg.rc, seg.oneway,
                                         [mid, seg.pts[1]]))
                    splits += 1
                    continue
            out.append(seg)
        elif extent <= max_extent_deg:
            out.append(seg)
        else:
            mid = len(seg.pts) // 2
            # Split into two sub-segments sharing the midpoint
            stack.append(Segment(seg.rc, seg.oneway, seg.pts[:mid + 1]))
            stack.append(Segment(seg.rc, seg.oneway, seg.pts[mid:]))
            splits += 1
    return out, splits


# ═══════════════════════════════════════════════════════════════════════════════
# Binary writer
# ═══════════════════════════════════════════════════════════════════════════════

def _assign_cell(seg, cell_e5, min_lat_e5, min_lon_e5, rows, cols):
    """Assign a segment to the grid cell of its midpoint."""
    mid = seg.pts[len(seg.pts) // 2]
    lat_e5 = to_e5(mid[0])
    lon_e5 = to_e5(mid[1])
    r = (lat_e5 - min_lat_e5) // cell_e5
    c = (lon_e5 - min_lon_e5) // cell_e5
    return (int(max(0, min(r, rows - 1))),
            int(max(0, min(c, cols - 1))))


def _encode_segment(seg):
    """Encode one segment to binary bytes."""
    pts_e5 = [(to_e5(lat), to_e5(lon)) for lat, lon in seg.pts]
    buf = struct.pack("<BBH", seg.rc, 1 if seg.oneway else 0, len(pts_e5))
    buf += struct.pack("<ii", pts_e5[0][0], pts_e5[0][1])
    prev = pts_e5[0]
    for lat, lon in pts_e5[1:]:
        dl = max(-32768, min(32767, lat - prev[0]))
        dn = max(-32768, min(32767, lon - prev[1]))
        buf += struct.pack("<hh", dl, dn)
        prev = (prev[0] + dl, prev[1] + dn)
    return buf


def write_bin(segments, output_path, cell_deg, tolerance_m):
    """Write the road_map.bin file.

    Returns a stats dict with file_size, segment/point counts, etc.
    """
    if not segments:
        print("  ERROR: no segments to write!", file=sys.stderr)
        return None

    # Compute bounding box from actual data (with small padding)
    all_lats = [p[0] for s in segments for p in s.pts]
    all_lons = [p[1] for s in segments for p in s.pts]
    min_lat = min(all_lats) - 0.01
    max_lat = max(all_lats) + 0.01
    min_lon = min(all_lons) - 0.01
    max_lon = max(all_lons) + 0.01

    min_lat_e5 = to_e5(min_lat)
    max_lat_e5 = to_e5(max_lat)
    min_lon_e5 = to_e5(min_lon)
    max_lon_e5 = to_e5(max_lon)
    cell_e5 = to_e5(cell_deg)

    rows = math.ceil((max_lat_e5 - min_lat_e5) / cell_e5)
    cols = math.ceil((max_lon_e5 - min_lon_e5) / cell_e5)

    # Assign each segment to its midpoint cell
    cell_map = {}  # (row, col) → [segment_index]
    for i, seg in enumerate(segments):
        rc = _assign_cell(seg, cell_e5, min_lat_e5, min_lon_e5, rows, cols)
        cell_map.setdefault(rc, []).append(i)

    total_pts = sum(len(s.pts) for s in segments)

    # Pre-encode all segments
    encoded = [_encode_segment(seg) for seg in segments]

    # Build grid index + contiguous segment data
    grid_index_offset = HEADER_SIZE
    grid_index_size = rows * cols * 8
    seg_data_offset = grid_index_offset + grid_index_size

    seg_data = bytearray()
    grid_idx = bytearray()

    for r in range(rows):
        for c in range(cols):
            indices = cell_map.get((r, c))
            if indices is None:
                grid_idx += struct.pack("<IHH", 0, 0, 0)
            else:
                off = len(seg_data)
                for idx in indices:
                    seg_data += encoded[idx]
                grid_idx += struct.pack("<IHH", off, len(indices), 0)

    file_size = seg_data_offset + len(seg_data)
    tolerance_cm = int(round(tolerance_m * 100))

    # Road class count (unique classes present)
    rc_set = {s.rc for s in segments}

    # Pack header (64 bytes)
    hdr = bytearray(HEADER_SIZE)
    struct.pack_into("<4sBBBB", hdr, 0,
                     MAGIC, FORMAT_VERSION, 0, len(rc_set), 0)
    struct.pack_into("<iiii", hdr, 8,
                     min_lat_e5, max_lat_e5, min_lon_e5, max_lon_e5)
    struct.pack_into("<HHi", hdr, 24,
                     rows, cols, cell_e5)
    struct.pack_into("<II", hdr, 32,
                     len(segments), total_pts)
    struct.pack_into("<HH", hdr, 40,
                     tolerance_cm, 0)
    struct.pack_into("<III", hdr, 44,
                     grid_index_offset, seg_data_offset, file_size)

    # Write
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(hdr)
        f.write(grid_idx)
        f.write(seg_data)

    return {
        "file_size": file_size,
        "segments": len(segments),
        "points": total_pts,
        "rows": rows,
        "cols": cols,
        "cells_used": len(cell_map),
        "cells_total": rows * cols,
        "grid_index_kb": grid_index_size / 1024,
        "seg_data_kb": len(seg_data) / 1024,
    }


# ═══════════════════════════════════════════════════════════════════════════════
# Verification (read-back sanity check)
# ═══════════════════════════════════════════════════════════════════════════════

def verify_bin(path):
    """Read back and verify the binary file structure."""
    with open(path, "rb") as f:
        hdr = f.read(HEADER_SIZE)

    if len(hdr) < HEADER_SIZE:
        print("  VERIFY FAIL: header too short", file=sys.stderr)
        return False

    magic = hdr[0:4]
    if magic != MAGIC:
        print(f"  VERIFY FAIL: bad magic {magic!r}", file=sys.stderr)
        return False

    version = hdr[4]
    if version != FORMAT_VERSION:
        print(f"  VERIFY FAIL: version {version} != {FORMAT_VERSION}",
              file=sys.stderr)
        return False

    min_lat, max_lat, min_lon, max_lon = struct.unpack_from("<iiii", hdr, 8)
    rows, cols, cell_e5 = struct.unpack_from("<HHi", hdr, 24)
    total_seg, total_pts = struct.unpack_from("<II", hdr, 32)
    tol_cm, _ = struct.unpack_from("<HH", hdr, 40)
    grid_off, seg_off, file_size = struct.unpack_from("<III", hdr, 44)

    actual_size = os.path.getsize(path)
    if actual_size != file_size:
        print(f"  VERIFY FAIL: size {actual_size} != header {file_size}",
              file=sys.stderr)
        return False

    print(f"  VERIFY OK: v{version}, {total_seg:,} segs, {total_pts:,} pts, "
          f"{rows}×{cols} grid, {tol_cm/100:.0f}m tol, "
          f"bbox ({min_lat/1e5:.2f},{min_lon/1e5:.2f})"
          f"→({max_lat/1e5:.2f},{max_lon/1e5:.2f})")
    return True


# ═══════════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════════

def main():
    ap = argparse.ArgumentParser(
        description="Generate road_map.bin for ESP32 lockout road-snapping")

    # ── Data source (mutually exclusive) ──────────────────────────────────
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--pbf", metavar="FILE",
                     help="OSM PBF file (download from Geofabrik)")
    src.add_argument("--download", action="store_true",
                     help="Auto-download PBF from Geofabrik (US or Colorado test)")
    src.add_argument("--overpass", action="store_true",
                     help="Legacy: fetch from Overpass API (slow, unreliable)")
    src.add_argument("--input", metavar="FILE",
                     help="Legacy: load from saved Overpass JSON")

    # ── Common options ────────────────────────────────────────────────────
    ap.add_argument("-o", "--output", default="road_map.bin",
                    help="Output file path (default: road_map.bin)")
    ap.add_argument("-t", "--tolerance", type=int, default=DEFAULT_TOLERANCE_M,
                    help=f"RDP tolerance in metres (default: {DEFAULT_TOLERANCE_M})")
    ap.add_argument("--cell-size", type=float, default=DEFAULT_CELL_DEG,
                    help=f"Grid cell size in degrees (default: {DEFAULT_CELL_DEG})")
    ap.add_argument("--region", choices=["us48", "test"], default="us48",
                    help="Geographic region (default: us48)")
    ap.add_argument("--classes", default="motorway,trunk",
                    help="Comma-separated road classes (default: motorway,trunk; "
                         "adding primary exceeds 8 MB PSRAM limit)")
    ap.add_argument("--pbf-dir", default=".road_map_cache",
                    help="Directory for downloaded PBF files (default: .road_map_cache)")
    ap.add_argument("--verify", action="store_true", default=True,
                    help="Verify output after writing (default: true)")
    ap.add_argument("--no-verify", action="store_false", dest="verify")
    ap.add_argument("--min-length", type=int, default=200,
                    help="Drop segments shorter than N metres (default: 200; "
                         "removes ramps/connectors to reduce file size)")

    # ── Legacy Overpass options ───────────────────────────────────────────
    ap.add_argument("--cache-dir", default=".road_map_cache",
                    help="Cache directory for Overpass responses")
    ap.add_argument("--chunk-lat", type=float, default=2.0,
                    help="Chunk latitude step for Overpass queries (default: 2.0)")
    ap.add_argument("--chunk-lon", type=float, default=5.0,
                    help="Chunk longitude step for Overpass queries (default: 5.0)")

    args = ap.parse_args()

    # If no data source specified, default to --download
    use_pbf = args.pbf or args.download
    use_overpass = args.overpass
    use_input = args.input
    if not use_pbf and not use_overpass and not use_input:
        use_pbf = True
        args.download = True

    # Parse allowed road classes
    allowed_classes = set()
    for cls in args.classes.split(","):
        cls = cls.strip().lower()
        if cls == "motorway":
            allowed_classes.add(RC_MOTORWAY)
        elif cls == "trunk":
            allowed_classes.add(RC_TRUNK)
        elif cls == "primary":
            allowed_classes.add(RC_PRIMARY)
        else:
            print(f"Unknown class: {cls}", file=sys.stderr)
            sys.exit(1)

    bbox = TEST_BBOX if args.region == "test" else US48_BBOX

    print("═" * 60)
    print("  Road Map Builder")
    print("═" * 60)
    print(f"  Region:    {args.region}  {bbox}")
    print(f"  Tolerance: {args.tolerance} m")
    print(f"  Cell size: {args.cell_size}°")
    print(f"  Classes:   {args.classes}")
    print(f"  Output:    {args.output}")
    print()

    # ── Step 1: Load road data ────────────────────────────────────────────

    segments = []

    if args.pbf:
        # PBF file provided directly
        pbf_path = args.pbf
        if not os.path.exists(pbf_path):
            print(f"ERROR: PBF file not found: {pbf_path}", file=sys.stderr)
            sys.exit(1)
        print(f"Loading from PBF: {pbf_path}")
        segments = _load_from_pbf(pbf_path, allowed_classes, bbox)

    elif args.download:
        # Auto-download from Geofabrik
        if args.region == "test":
            url = GEOFABRIK_CO_URL
            filename = "colorado-latest.osm.pbf"
        else:
            url = GEOFABRIK_US_URL
            filename = "us-latest.osm.pbf"
        pbf_path = os.path.join(args.pbf_dir, filename)
        print(f"Downloading PBF from Geofabrik ...")
        _download_pbf(url, pbf_path)
        print(f"\nLoading from PBF: {pbf_path}")
        segments = _load_from_pbf(pbf_path, allowed_classes, bbox)

    elif use_input:
        # Legacy: Overpass JSON file
        print(f"Loading from JSON: {args.input} ...")
        seen_ids = set()
        with open(args.input, "r") as f:
            data = json.load(f)
        elements = data if isinstance(data, list) else data.get("elements", [])
        segments = _parse_elements(elements, seen_ids, allowed_classes)
        print(f"  Parsed {len(segments):,} ways")

    elif use_overpass:
        # Legacy: Overpass API download
        seen_ids = set()
        if args.region == "test":
            chunks = [bbox]
        else:
            chunks = _make_chunks(bbox, args.chunk_lat, args.chunk_lon)
        print(f"Downloading {len(chunks)} chunks from Overpass API ...")
        print(f"  (cached responses in {args.cache_dir}/)")
        print()
        failed_chunks = 0
        for i, chunk in enumerate(chunks):
            print(f"  [{i+1}/{len(chunks)}]", end="")
            results = _fetch_chunk_adaptive(chunk, args.cache_dir)
            if not results:
                failed_chunks += 1
            for data in results:
                new_segs = _parse_elements(
                    data.get("elements", []), seen_ids, allowed_classes)
                segments.extend(new_segs)
            if segments:
                print(f"         total: {len(segments):,}")
            if i < len(chunks) - 1:
                time.sleep(CHUNK_PAUSE_S)
        if failed_chunks:
            print(f"\n  ⚠  {failed_chunks} chunk(s) failed — "
                  f"re-run to retry (cached chunks are preserved)")

    if not segments:
        print("\nERROR: No road segments found!", file=sys.stderr)
        sys.exit(1)

    orig_count = len(segments)
    orig_pts = sum(len(s.pts) for s in segments)
    print(f"\n{'─' * 60}")
    print(f"  Raw data: {orig_count:,} segments, {orig_pts:,} points")

    # ── Step 2: Merge adjacent ways ───────────────────────────────────────

    print(f"  Merging adjacent ways ...")
    t0 = time.monotonic()
    segments, merges = _merge_ways(segments)
    dt = time.monotonic() - t0
    merge_pts = sum(len(s.pts) for s in segments)
    print(f"  → {len(segments):,} segments, {merge_pts:,} points "
          f"({merges:,} merges, {dt:.1f}s)")

    # ── Step 2b: Drop short segments ───────────────────────────────────────

    if args.min_length > 0:
        print(f"  Dropping segments < {args.min_length}m ...")
        segments, short_dropped = _drop_short(segments, args.min_length)
        ds_pts = sum(len(s.pts) for s in segments)
        print(f"  → {len(segments):,} segments, {ds_pts:,} points "
              f"(dropped {short_dropped:,} short segments)")

    # ── Step 3: Simplify ──────────────────────────────────────────────────

    print(f"  Simplifying (tolerance={args.tolerance}m) ...")
    t0 = time.monotonic()
    segments, dropped = _simplify_all(segments, args.tolerance)
    dt = time.monotonic() - t0
    simp_pts = sum(len(s.pts) for s in segments)
    ratio = merge_pts / max(1, simp_pts)
    print(f"  → {len(segments):,} segments, {simp_pts:,} points "
          f"(dropped {dropped}, ratio {ratio:.1f}×, {dt:.1f}s)")

    # ── Step 4: Split long segments ───────────────────────────────────────

    max_extent = args.cell_size * 0.9
    segments, splits = _split_long(segments, max_extent)
    if splits:
        final_pts = sum(len(s.pts) for s in segments)
        print(f"  → {splits} segments split (now {len(segments):,} segments, "
              f"{final_pts:,} points)")

    # ── Step 5: Write binary ──────────────────────────────────────────────

    print(f"\n  Writing {args.output} ...")
    stats = write_bin(segments, args.output, args.cell_size, args.tolerance)

    if stats is None:
        sys.exit(1)

    print()
    print("═" * 60)
    print(f"  Output:       {args.output}")
    print(f"  File size:    {stats['file_size']:>12,} bytes "
          f"({stats['file_size']/1024/1024:.2f} MB)")
    print(f"  Segments:     {stats['segments']:>12,}")
    print(f"  Points:       {stats['points']:>12,}")
    print(f"  Grid:         {stats['rows']}×{stats['cols']} "
          f"({stats['cells_used']:,}/{stats['cells_total']:,} cells used)")
    print(f"  Grid index:   {stats['grid_index_kb']:>10.1f} KB")
    print(f"  Segment data: {stats['seg_data_kb']:>10.1f} KB")
    print("═" * 60)

    # ── Step 6: Verify ────────────────────────────────────────────────────

    if args.verify:
        verify_bin(args.output)

    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
