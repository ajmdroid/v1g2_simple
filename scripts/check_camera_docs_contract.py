#!/usr/bin/env python3
"""Contract checks for retired camera docs surfaces."""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
API_DOC = ROOT / "docs" / "API.md"
MANUAL_DOC = ROOT / "docs" / "MANUAL.md"
ROAD_MAP_DOC = ROOT / "docs" / "ROAD_MAP_FORMAT.md"


def assert_contains(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label}: missing required text {needle!r}")


def assert_not_contains(text: str, needle: str, label: str) -> None:
    if needle in text:
        raise AssertionError(f"{label}: unexpected stale text {needle!r}")


def main() -> int:
    api_text = API_DOC.read_text(encoding="utf-8")
    manual_text = MANUAL_DOC.read_text(encoding="utf-8")
    road_map_text = ROAD_MAP_DOC.read_text(encoding="utf-8")

    assert_not_contains(api_text, "[Cameras](#cameras)", "docs/API.md")
    assert_not_contains(api_text, "/api/cameras", "docs/API.md")

    assert_not_contains(road_map_text, "## Camera Data", "docs/ROAD_MAP_FORMAT.md")
    assert_not_contains(road_map_text, "### Camera Lookup", "docs/ROAD_MAP_FORMAT.md")
    assert_not_contains(road_map_text, "--cameras", "docs/ROAD_MAP_FORMAT.md")
    assert_not_contains(road_map_text, "ALPR overlay", "docs/ROAD_MAP_FORMAT.md")
    assert_contains(road_map_text, "Legacy reserved field, currently `0`", "docs/ROAD_MAP_FORMAT.md")

    assert_not_contains(manual_text, "camera, volume_fade", "docs/MANUAL.md")
    assert_not_contains(manual_text, "camera-overlap coverage", "docs/MANUAL.md")

    print("[contract] retired camera docs surfaces removed from tracked docs")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[contract] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
