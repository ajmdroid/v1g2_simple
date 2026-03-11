#!/usr/bin/env python3
"""Generate a minimal road_map.bin fixture with ALPR-only camera data.

Creates a valid v2 binary with one road segment and 1 ALPR camera record
at a known coordinate
near Denver, CO.  Used to validate the firmware camera reader contract.

Usage:
    python scripts/generate_camera_fixture.py [output]

Default output: test/fixtures/camera_types_road_map.bin
"""

import struct
import sys
from pathlib import Path

MAGIC = b"RMAP"
FORMAT_VERSION = 2
HEADER_SIZE = 64

# Camera type flags — must match build_road_map.py and firmware
CAM_TYPE_ALPR = 4


def to_e5(deg):
    return int(round(deg * 100000))


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else "test/fixtures/camera_types_road_map.bin"

    # --- Fixture data ---
    # A short road segment on I-25 near Denver
    seg_pts = [
        (39.7392, -104.9903),  # Denver center
        (39.7500, -104.9903),  # ~1.2 km north
    ]
    seg_rc = 0       # motorway
    seg_oneway = 1
    seg_speed = 65   # mph

    # Single ALPR camera within search range
    cameras = [
        (39.7460, -104.9905, 0xFFFF, CAM_TYPE_ALPR, 30),
    ]

    # --- Bounding box (with padding) ---
    all_lats = [p[0] for p in seg_pts] + [c[0] for c in cameras]
    all_lons = [p[1] for p in seg_pts] + [c[1] for c in cameras]
    min_lat_e5 = to_e5(min(all_lats) - 0.01)
    max_lat_e5 = to_e5(max(all_lats) + 0.01)
    min_lon_e5 = to_e5(min(all_lons) - 0.01)
    max_lon_e5 = to_e5(max(all_lons) + 0.01)

    cell_e5 = to_e5(0.5)  # 0.5° cells — one cell covers everything
    rows = 1
    cols = 1

    # --- Encode segment (v2) ---
    pts_e5 = [(to_e5(lat), to_e5(lon)) for lat, lon in seg_pts]
    seg_buf = struct.pack("<BBH", seg_rc, seg_oneway, len(pts_e5))
    seg_buf += struct.pack("<ii", pts_e5[0][0], pts_e5[0][1])
    prev = pts_e5[0]
    for lat_e5, lon_e5 in pts_e5[1:]:
        dl = lat_e5 - prev[0]
        dn = lon_e5 - prev[1]
        seg_buf += struct.pack("<hh", dl, dn)
        prev = (prev[0] + dl, prev[1] + dn)
    seg_buf += struct.pack("<B", seg_speed)  # v2 trailing speed byte

    # --- Grid layout ---
    grid_index_offset = HEADER_SIZE
    grid_index_size = rows * cols * 8
    seg_data_offset = grid_index_offset + grid_index_size

    # Road grid: single cell with 1 segment
    road_grid = struct.pack("<IHH", 0, 1, 0)
    seg_data = seg_buf

    # Camera grid + data
    cam_grid_offset = seg_data_offset + len(seg_data)
    cam_count = len(cameras)

    cam_grid = struct.pack("<IHH", 0, cam_count, 0)
    cam_data = bytearray()
    for lat, lon, brg, flg, spd in cameras:
        cam_data += struct.pack("<iiHBB",
                                to_e5(lat), to_e5(lon), brg, flg, spd)

    file_size = cam_grid_offset + len(cam_grid) + len(cam_data)

    # --- Header ---
    hdr = bytearray(HEADER_SIZE)
    struct.pack_into("<4sBBBB", hdr, 0, MAGIC, FORMAT_VERSION, 0, 1, 0)
    struct.pack_into("<iiii", hdr, 8,
                     min_lat_e5, max_lat_e5, min_lon_e5, max_lon_e5)
    struct.pack_into("<HHi", hdr, 24, rows, cols, cell_e5)
    struct.pack_into("<II", hdr, 32, 1, len(pts_e5))  # 1 segment, N points
    struct.pack_into("<HH", hdr, 40, 10000, 0)  # tolerance 100m, reserved
    struct.pack_into("<III", hdr, 44,
                     grid_index_offset, seg_data_offset, file_size)
    struct.pack_into("<II", hdr, 56, cam_grid_offset, cam_count)

    # --- Write ---
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    with open(output, "wb") as f:
        f.write(hdr)
        f.write(road_grid)
        f.write(seg_data)
        f.write(cam_grid)
        f.write(cam_data)

    print(f"  Wrote {output} ({file_size} bytes)")
    print(f"  Segments: 1, Points: {len(pts_e5)}")
    print(f"  Cameras:  {cam_count} (alpr={CAM_TYPE_ALPR})")

    # --- Verify by reading back ---
    with open(output, "rb") as f:
        data = f.read()

    magic = data[0:4]
    ver = data[4]
    cam_off = struct.unpack_from("<I", data, 56)[0]
    cam_cnt = struct.unpack_from("<I", data, 60)[0]
    assert magic == MAGIC, f"Bad magic: {magic}"
    assert ver == FORMAT_VERSION, f"Bad version: {ver}"
    assert cam_cnt == cam_count, f"Camera count mismatch: {cam_cnt} != {cam_count}"
    assert cam_off == cam_grid_offset, f"Camera offset mismatch"

    # Read back each camera record
    cam_data_start = cam_off + rows * cols * 8
    for i in range(cam_cnt):
        off = cam_data_start + i * 12
        lat_e5, lon_e5, brg, flg, spd = struct.unpack_from("<iiHBB", data, off)
        expected = cameras[i]
        assert flg == expected[3], f"Camera {i} flags: {flg} != {expected[3]}"
        assert flg == CAM_TYPE_ALPR

    print("  Verification: PASS — ALPR-only camera fixture present and valid")


if __name__ == "__main__":
    main()
