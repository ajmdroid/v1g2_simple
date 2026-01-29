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

# Camera types matching ESP32 enum
CAMERA_TYPES = {
    1: 1,  # RedLightAndSpeed
    2: 2,  # SpeedCamera
    3: 3,  # RedLightCamera
    8192: 4,  # ALPR
}


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path, help="Input enriched NDJSON file")
    p.add_argument("output", type=Path, help="Output binary file (.bin)")
    return p.parse_args()


def convert_camera(obj: dict) -> bytes:
    """Convert single camera record to 24-byte binary."""
    lat = obj.get("lat", 0.0)
    lon = obj.get("lon", 0.0)
    
    # Snap point (midpoint of corridor endpoints, or camera location if no corridor)
    p1 = obj.get("p1", [lat, lon])
    p2 = obj.get("p2", [lat, lon])
    p1_lat = p1[0] if len(p1) >= 2 else lat
    p1_lon = p1[1] if len(p1) >= 2 else lon
    p2_lat = p2[0] if len(p2) >= 2 else lat
    p2_lon = p2[1] if len(p2) >= 2 else lon
    
    # Snap point is midpoint of corridor
    snap_lat = (p1_lat + p2_lat) / 2.0
    snap_lon = (p1_lon + p2_lon) / 2.0
    
    # Bearing (stored as bearing * 10 to preserve one decimal place)
    brg = obj.get("brg", -1)
    if brg is None:
        brg = -1
    else:
        brg = int(brg * 10)  # Multiply by 10 for one decimal precision
    
    # Corridor width
    width = obj.get("w", 35)
    if width is None:
        width = 35
    width = min(255, max(0, int(width)))
    
    # Bearing tolerance (default 30 degrees)
    tolerance = 30
    
    # Type
    flg = obj.get("flg", 2)
    cam_type = CAMERA_TYPES.get(flg, 2)
    
    # Speed
    speed = obj.get("spd", 0)
    if speed is None:
        speed = 0
    speed = min(255, max(0, int(speed)))
    
    # Flags
    flags = 0
    if obj.get("unt") == "kmh":
        flags |= 0x01
    
    # Pack: 4 floats + 1 int16 + 6 uint8
    return struct.pack(
        "<4f h 6B",
        lat, lon,
        snap_lat, snap_lon,
        brg,
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
