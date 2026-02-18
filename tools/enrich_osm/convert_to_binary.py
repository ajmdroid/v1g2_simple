#!/usr/bin/env python3
"""Convert enriched NDJSON camera files to compact binary format.

Binary format is ~24 bytes per camera vs ~200+ bytes JSON, and loads 10-20x faster
on ESP32 since there's no JSON parsing overhead.

Format:
  Header (16 bytes):
    magic: 4 bytes "VCAM"
    version: 4 bytes uint32 (1)
    count: 4 bytes uint32 (camera count)
    recordSize: 4 bytes uint32 (24)
    
  Camera records (24 bytes each):
    lat: 4 bytes float
    lon: 4 bytes float
    snapLat: 4 bytes float (road snap point)
    snapLon: 4 bytes float (road snap point)
    bearing: 2 bytes int16 (road bearing * 10, -1 = unknown)
    width: 1 byte (corridor half-width meters)
    tolerance: 1 byte (bearing tolerance degrees)
    type: 1 byte (CameraType enum)
    speed: 1 byte (speed limit, 0 = unknown)
    flags: 1 byte (bit 0 = metric)
    reserved: 1 byte
"""

import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = b"VCAM"
VERSION = 1
RECORD_SIZE = 24

CAMERA_TYPE_ALPR = 4


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path, help="Input enriched NDJSON file")
    p.add_argument("output", type=Path, help="Output binary file (.bin)")
    return p.parse_args()


def _parse_float(value, fallback):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float(fallback)


def _parse_int(value, fallback):
    try:
        return int(value)
    except (TypeError, ValueError):
        return int(fallback)


def convert_camera(obj: dict) -> bytes:
    """Convert single camera record to 24-byte binary."""
    lat = _parse_float(obj.get("lat", 0.0), 0.0)
    lon = _parse_float(obj.get("lon", 0.0), 0.0)

    # Snap point sources, in priority order:
    # 1) Enriched corridor endpoints (p1/p2)
    # 2) Canonical snap keys (slt/sln)
    # 3) Legacy snap keys (slat/slon)
    # 4) Camera lat/lon fallback
    p1 = obj.get("p1")
    p2 = obj.get("p2")
    if isinstance(p1, (list, tuple)) and isinstance(p2, (list, tuple)) and len(p1) >= 2 and len(p2) >= 2:
        p1_lat = _parse_float(p1[0], lat)
        p1_lon = _parse_float(p1[1], lon)
        p2_lat = _parse_float(p2[0], lat)
        p2_lon = _parse_float(p2[1], lon)
        snap_lat = (p1_lat + p2_lat) * 0.5
        snap_lon = (p1_lon + p2_lon) * 0.5
    else:
        snap_lat = _parse_float(obj.get("slt", obj.get("slat", lat)), lat)
        snap_lon = _parse_float(obj.get("sln", obj.get("slon", lon)), lon)

    # Bearing stored as *10 (0.1 degree precision), -1 sentinel if unknown.
    raw_bearing = obj.get("rbr", obj.get("brg"))
    if raw_bearing is None:
        bearing = -1
    else:
        bearing = int(round(_parse_float(raw_bearing, -1) * 10.0))
        if bearing < -1:
            bearing = -1
        elif bearing > 3599:
            bearing = 3599

    # Corridor width/tolerance defaults should mirror downloader defaults.
    width = min(255, max(0, _parse_int(obj.get("cwm", obj.get("w", 35)), 35)))
    tolerance = min(255, max(0, _parse_int(obj.get("btol", 30), 30)))

    # ALPR-only runtime schema.
    cam_type = CAMERA_TYPE_ALPR

    # Speed limit value (support either spd or psl metadata).
    speed = min(255, max(0, _parse_int(obj.get("spd", obj.get("psl", 0)), 0)))

    # Runtime flags.
    flags = 0
    if obj.get("unt") == "kmh":
        flags |= 0x01

    # Pack: 4 floats + 1 int16 + 6 uint8
    return struct.pack(
        "<4f h 6B",
        lat, lon,
        snap_lat, snap_lon,
        bearing,
        width, tolerance, cam_type, speed, flags, 0  # last is reserved
    )


def main():
    args = parse_args()
    
    cameras = []
    with args.input.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                # Skip metadata lines
                if "_meta" in obj:
                    continue
                cameras.append(obj)
            except json.JSONDecodeError:
                continue
    
    print(f"Converting {len(cameras)} cameras to binary...")
    
    # Write header (16 bytes)
    # Magic (4) + Version (4) + Count (4) + RecordSize (4)
    header = struct.pack(
        "<4s I I I",
        MAGIC,
        VERSION,
        len(cameras),
        RECORD_SIZE
    )
    
    with args.output.open("wb") as out:
        out.write(header)
        for cam in cameras:
            out.write(convert_camera(cam))
    
    input_size = args.input.stat().st_size
    output_size = args.output.stat().st_size
    ratio = output_size / input_size * 100
    
    print(f"Input:  {input_size:,} bytes ({input_size/1024:.1f} KB)")
    print(f"Output: {output_size:,} bytes ({output_size/1024:.1f} KB)")
    print(f"Ratio:  {ratio:.1f}% ({100-ratio:.0f}% smaller)")
    print(f"Written to {args.output}")


if __name__ == "__main__":
    main()
