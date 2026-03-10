#!/usr/bin/env python3
"""Verify deployed data/audio clips match the curated manifest exactly."""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ROOT / "config" / "audio_asset_manifest.json"
AUDIO_DIR = ROOT / "data" / "audio"


def expand_manifest() -> list[str]:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    files = set(manifest.get("files", []))
    for entry in manifest.get("ranges", []):
        prefix = entry.get("prefix", "")
        suffix = entry.get("suffix", "")
        width = int(entry.get("width", 0))
        start = int(entry.get("start", 0))
        end = int(entry.get("end", -1))
        for value in range(start, end + 1):
            files.add(f"{prefix}{value:0{width}d}{suffix}")
    return sorted(files)


def main() -> int:
    if not MANIFEST_PATH.exists():
        print(f"[audio-manifest] manifest not found: {MANIFEST_PATH}")
        return 1

    if not AUDIO_DIR.exists():
        print(f"[audio-manifest] audio directory not found: {AUDIO_DIR}")
        return 1

    expected = expand_manifest()
    actual = sorted(path.name for path in AUDIO_DIR.glob("*.mul"))

    missing = sorted(set(expected) - set(actual))
    unexpected = sorted(set(actual) - set(expected))

    if missing or unexpected:
        if missing:
            print(f"[audio-manifest] missing clips ({len(missing)}):")
            for name in missing:
                print(f"  - {name}")
        if unexpected:
            print(f"[audio-manifest] unexpected clips ({len(unexpected)}):")
            for name in unexpected:
                print(f"  - {name}")
        print("[audio-manifest] deployed audio clips do not match manifest")
        return 1

    print(
        "[audio-manifest] deployed audio clips match manifest "
        f"({len(actual)} clips under data/audio)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
