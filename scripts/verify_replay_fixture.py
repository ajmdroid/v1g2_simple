#!/usr/bin/env python3
"""Validate a replay fixture directory against the required schema.

Checks:
  - Required files exist: meta.json, packets.csv, expected.json
  - meta.json has required keys and valid lane tag
  - CSV timestamps are monotonically non-decreasing
  - packets.csv has required columns
  - expected.json is valid JSON

Usage:
    python3 scripts/verify_replay_fixture.py test/fixtures/replay/my_scenario/

Exit codes:
    0 = valid fixture
    1 = validation failure(s)
    2 = usage error
"""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

REQUIRED_FILES = ["meta.json", "packets.csv", "expected.json"]
REQUIRED_META_KEYS = ["scenario_id", "owner", "lane", "sanitization_version"]
VALID_LANES = {"pr", "nightly"}
PACKETS_COLS = {"timestamp_ms", "frame_hex"}


def check_monotonic_timestamps(path: Path) -> list[str]:
    """Return errors if timestamp_ms column is not monotonically non-decreasing."""
    errors = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None or "timestamp_ms" not in reader.fieldnames:
            return [f"{path.name}: missing timestamp_ms column"]
        prev = -1
        for i, row in enumerate(reader, start=2):  # line 2 = first data row
            try:
                ts = int(row["timestamp_ms"])
            except (ValueError, KeyError):
                errors.append(f"{path.name}:{i}: invalid timestamp_ms")
                continue
            if ts < prev:
                errors.append(
                    f"{path.name}:{i}: timestamp {ts} < previous {prev} (not monotonic)"
                )
            prev = ts
    return errors


def validate_fixture(fixture_dir: Path) -> list[str]:
    errors = []

    # Check required files
    for fname in REQUIRED_FILES:
        if not (fixture_dir / fname).exists():
            errors.append(f"Missing required file: {fname}")

    if errors:
        return errors  # Can't continue without files

    # Validate meta.json
    try:
        with open(fixture_dir / "meta.json") as f:
            meta = json.load(f)
    except json.JSONDecodeError as e:
        errors.append(f"meta.json: invalid JSON: {e}")
        meta = {}

    for key in REQUIRED_META_KEYS:
        if key not in meta:
            errors.append(f"meta.json: missing required key '{key}'")

    if meta.get("lane") and meta["lane"] not in VALID_LANES:
        errors.append(
            f"meta.json: invalid lane '{meta['lane']}', must be one of {VALID_LANES}"
        )

    # Validate expected.json
    try:
        with open(fixture_dir / "expected.json") as f:
            json.load(f)
    except json.JSONDecodeError as e:
        errors.append(f"expected.json: invalid JSON: {e}")

    # Validate packets.csv columns
    with open(fixture_dir / "packets.csv", newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            errors.append("packets.csv: empty file")
        else:
            missing = PACKETS_COLS - set(reader.fieldnames)
            if missing:
                errors.append(f"packets.csv: missing columns: {missing}")

    # Check monotonic timestamps
    errors.extend(check_monotonic_timestamps(fixture_dir / "packets.csv"))

    return errors


def main() -> int:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <fixture-dir> [<fixture-dir> ...]", file=sys.stderr)
        return 2

    all_clean = True
    for arg in sys.argv[1:]:
        fixture_dir = Path(arg)
        if not fixture_dir.is_dir():
            print(f"Not a directory: {fixture_dir}", file=sys.stderr)
            all_clean = False
            continue

        errors = validate_fixture(fixture_dir)
        if errors:
            print(f"FAIL: {fixture_dir}")
            for e in errors:
                print(f"  - {e}")
            all_clean = False
        else:
            print(f"OK: {fixture_dir}")

    return 0 if all_clean else 1


if __name__ == "__main__":
    sys.exit(main())
