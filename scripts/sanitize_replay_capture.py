#!/usr/bin/env python3
"""Sanitize a raw captured log into a committed replay fixture.

Transforms raw capture data so it can be committed safely:
  - Translates GPS coordinates to a local origin (preserves distances/headings)
  - Converts absolute timestamps to relative offsets from first sample
  - Strips device/user identifiers
  - Emits meta.json with sanitization_version

Usage:
    python3 scripts/sanitize_replay_capture.py \\
        --raw-dir /path/to/raw_capture/ \\
        --out-dir test/fixtures/replay/my_scenario/ \\
        --scenario-id my_scenario \\
        --owner lockout \\
        --lane pr

Input directory expects:
    packets.csv   – timestamp_ms,frame_hex[,device_id,...]
    gps.csv       – timestamp_ms,lat,lon,speed_mph,course_deg,has_fix[,device_id,...]

Extra columns beyond the required ones are silently dropped.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

SANITIZATION_VERSION = 1

PACKETS_REQUIRED_COLS = ["timestamp_ms", "frame_hex"]
GPS_REQUIRED_COLS = ["timestamp_ms", "lat", "lon", "speed_mph", "course_deg", "has_fix"]


def translate_coords(
    rows: list[dict], origin_lat: float, origin_lon: float
) -> list[dict]:
    """Shift all lat/lon values so the first sample becomes (0,0) while
    preserving inter-sample distances and headings."""
    for row in rows:
        row["lat"] = str(float(row["lat"]) - origin_lat)
        row["lon"] = str(float(row["lon"]) - origin_lon)
    return rows


def relativize_timestamps(rows: list[dict]) -> list[dict]:
    """Convert absolute timestamps to offsets from the first sample."""
    if not rows:
        return rows
    base = int(rows[0]["timestamp_ms"])
    for row in rows:
        row["timestamp_ms"] = str(int(row["timestamp_ms"]) - base)
    return rows


def strip_extra_columns(rows: list[dict], keep: list[str]) -> list[dict]:
    """Keep only the required columns, dropping device ids etc."""
    return [{k: row[k] for k in keep if k in row} for row in rows]


def read_csv(path: Path, required: list[str]) -> list[dict]:
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            print(f"Empty CSV: {path}", file=sys.stderr)
            sys.exit(2)
        missing = set(required) - set(reader.fieldnames)
        if missing:
            print(f"Missing columns in {path}: {missing}", file=sys.stderr)
            sys.exit(2)
        return list(reader)


def write_csv(path: Path, rows: list[dict], fieldnames: list[str]) -> None:
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--raw-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--scenario-id", required=True)
    parser.add_argument("--source-capture-id", default="unknown")
    parser.add_argument("--owner", required=True,
                        help="Owning subsystem (e.g. lockout, camera, parser)")
    parser.add_argument("--lane", required=True, choices=["pr", "nightly"])
    args = parser.parse_args()

    raw_packets = args.raw_dir / "packets.csv"
    raw_gps = args.raw_dir / "gps.csv"

    if not raw_packets.exists():
        print(f"Missing {raw_packets}", file=sys.stderr)
        return 2
    if not raw_gps.exists():
        print(f"Missing {raw_gps}", file=sys.stderr)
        return 2

    packets = read_csv(raw_packets, PACKETS_REQUIRED_COLS)
    gps = read_csv(raw_gps, GPS_REQUIRED_COLS)

    # Sanitize GPS
    if gps:
        origin_lat = float(gps[0]["lat"])
        origin_lon = float(gps[0]["lon"])
    else:
        origin_lat = origin_lon = 0.0

    gps = strip_extra_columns(gps, GPS_REQUIRED_COLS)
    gps = translate_coords(gps, origin_lat, origin_lon)
    gps = relativize_timestamps(gps)

    # Sanitize packets
    packets = strip_extra_columns(packets, PACKETS_REQUIRED_COLS)
    packets = relativize_timestamps(packets)

    # Write outputs
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.out_dir / "packets.csv", packets, PACKETS_REQUIRED_COLS)
    write_csv(args.out_dir / "gps.csv", gps, GPS_REQUIRED_COLS)

    meta = {
        "scenario_id": args.scenario_id,
        "source_capture_id": args.source_capture_id,
        "owner": args.owner,
        "lane": args.lane,
        "sanitization_version": SANITIZATION_VERSION,
    }
    with open(args.out_dir / "meta.json", "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    # Create a placeholder expected.json if it doesn't exist
    expected_path = args.out_dir / "expected.json"
    if not expected_path.exists():
        placeholder = {
            "_comment": "Fill in assertions for this replay scenario",
            "alerts": [],
            "mute_windows": [],
            "lockout_transitions": [],
        }
        with open(expected_path, "w") as f:
            json.dump(placeholder, f, indent=2)
            f.write("\n")
        print(f"Created placeholder {expected_path} — fill in assertions manually")

    print(f"Sanitized fixture written to {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
