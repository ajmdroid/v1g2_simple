#!/usr/bin/env python3
"""
ALPR Camera Database Generator for V1 Simple

Downloads ALPR camera data from OpenStreetMap and generates an NDJSON file.
Output files go in the selected output directory for easy SD card copy.

Usage:
    python3 tools/update_cameras.py
    python3 tools/update_cameras.py --type alpr
    python3 tools/update_cameras.py --output ./camera_data

Output files (NDJSON format):
    alpr.json
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

try:
    import requests
except ImportError:
    print("Error: 'requests' module not found. Install with: pip3 install requests")
    sys.exit(1)

OVERPASS_URL = "https://overpass-api.de/api/interpreter"
CAMERA_FLAG_ALPR = 4


def log(msg: str) -> None:
    print(f"[cameras] {msg}")


def download_alpr_from_osm() -> list:
    """Download ALPR cameras from OpenStreetMap via Overpass API."""
    log("Downloading ALPR cameras from OpenStreetMap...")

    query = """
    [out:json][timeout:180];
    area["ISO3166-1"="US"]->.usa;
    (
      node["man_made"="surveillance"]["surveillance:type"="ALPR"](area.usa);
      node["man_made"="surveillance"]["surveillance"="ALPR"](area.usa);
      node["surveillance:type"="ALPR"](area.usa);
      way["surveillance:type"="ALPR"](area.usa);
    );
    out center;
    """

    try:
        response = requests.post(
            OVERPASS_URL,
            data={"data": query},
            timeout=300,
            headers={"User-Agent": "V1Simple/1.0 (ALPR camera database generator)"},
        )
        response.raise_for_status()
        data = response.json()
    except requests.exceptions.Timeout:
        log("  ✗ Overpass API timeout (try again later)")
        return []
    except Exception as exc:
        log(f"  ✗ Error: {exc}")
        return []

    cameras = []
    seen = set()

    for element in data.get("elements", []):
        if element.get("type") == "way" and "center" in element:
            lat = element["center"].get("lat")
            lon = element["center"].get("lon")
        else:
            lat = element.get("lat")
            lon = element.get("lon")

        if lat is None or lon is None:
            continue

        key = f"{lat:.5f},{lon:.5f}"
        if key in seen:
            continue
        seen.add(key)

        cameras.append(
            {
                "lat": round(lat, 6),
                "lon": round(lon, 6),
                "flg": CAMERA_FLAG_ALPR,
            }
        )

    log(f"  ✓ {len(cameras)} ALPR cameras")
    return cameras


def save_cameras(cameras: list, filepath: Path) -> bool:
    """Save cameras to NDJSON file."""
    if not cameras:
        log("  ⚠ No ALPR cameras to save")
        return False

    with open(filepath, "w") as handle:
        for camera in cameras:
            handle.write(json.dumps(camera, separators=(",", ":")) + "\n")

    size_kb = filepath.stat().st_size / 1024.0
    log(f"  → Saved {filepath.name} ({len(cameras)} cameras, {size_kb:.1f} KB)")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate V1 Simple ALPR camera database file")
    parser.add_argument(
        "--type",
        choices=["alpr"],
        default="alpr",
        help="Camera type to download (ALPR only)",
    )
    parser.add_argument(
        "--output",
        type=str,
        default=".",
        help="Output directory (default: current directory)",
    )
    args = parser.parse_args()

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    today = datetime.now().strftime("%Y-%m-%d")
    log(f"ALPR Camera Database Generator - {today}")
    log(f"Output directory: {output_dir.absolute()}")
    print()

    cameras = download_alpr_from_osm()
    ok = save_cameras(cameras, output_dir / "alpr.json")

    print()
    if ok:
        log(f"✅ Generated ALPR file in {output_dir}")
        log("Copy alpr.json to your SD prep pipeline input")
        return 0

    log("❌ No file generated - check network connection")
    return 1


if __name__ == "__main__":
    sys.exit(main())
