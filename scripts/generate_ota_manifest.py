#!/usr/bin/env python3
"""
generate_ota_manifest.py — Build an ota-manifest.json for GitHub Releases.

Called by the release workflow after firmware and filesystem images are built.
The manifest tells devices whether they can upgrade, what to download, and
how to verify the download.

Usage:
    python3 scripts/generate_ota_manifest.py \
        --version 4.1.0 \
        --firmware release/firmware.bin \
        --filesystem release/littlefs.bin \
        --output release/ota-manifest.json

Optional:
    --min-from-version 3.8.0   Oldest firmware that can upgrade directly.
                                Defaults to "0.0.0" (any version can upgrade).
    --breaking                  Flag this release as breaking (UI shows warning).
    --changelog "text"          Short human-readable changelog for the update banner.
    --notes "text"              Freeform notes (e.g., "Requires BLE re-pair").
"""

import argparse
import hashlib
import json
import os
import sys


def sha256_file(path: str) -> str:
    """Compute the SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def file_size(path: str) -> int:
    """Return file size in bytes."""
    return os.path.getsize(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate ota-manifest.json for OTA updates via GitHub Releases."
    )
    parser.add_argument(
        "--version", required=True,
        help="Semver version string for this release (e.g., 4.1.0)."
    )
    parser.add_argument(
        "--firmware", required=True,
        help="Path to firmware.bin."
    )
    parser.add_argument(
        "--filesystem", required=True,
        help="Path to littlefs.bin."
    )
    parser.add_argument(
        "--output", required=True,
        help="Path to write ota-manifest.json."
    )
    parser.add_argument(
        "--min-from-version", default="0.0.0",
        help="Oldest firmware version that can upgrade directly to this release. "
             "Default: 0.0.0 (no restriction)."
    )
    parser.add_argument(
        "--breaking", action="store_true", default=False,
        help="Mark this release as breaking (UI shows confirmation warning)."
    )
    parser.add_argument(
        "--changelog", default="",
        help="Short changelog text shown in the update banner."
    )
    parser.add_argument(
        "--notes", default="",
        help="Freeform notes for edge cases."
    )
    return parser.parse_args()


def validate_inputs(args: argparse.Namespace) -> None:
    """Validate that input files exist and version looks like semver."""
    if not os.path.isfile(args.firmware):
        print(f"ERROR: firmware file not found: {args.firmware}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.filesystem):
        print(f"ERROR: filesystem file not found: {args.filesystem}", file=sys.stderr)
        sys.exit(1)

    # Basic semver check (MAJOR.MINOR.PATCH)
    parts = args.version.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        print(f"ERROR: version must be semver (got: {args.version})", file=sys.stderr)
        sys.exit(1)

    parts = args.min_from_version.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        print(f"ERROR: min-from-version must be semver (got: {args.min_from_version})",
              file=sys.stderr)
        sys.exit(1)


def main() -> None:
    args = parse_args()
    validate_inputs(args)

    fw_name = os.path.basename(args.firmware)
    fs_name = os.path.basename(args.filesystem)

    manifest = {
        "version": args.version,
        "min_from_version": args.min_from_version,
        "firmware": {
            "file": fw_name,
            "size": file_size(args.firmware),
            "sha256": sha256_file(args.firmware),
        },
        "filesystem": {
            "file": fs_name,
            "size": file_size(args.filesystem),
            "sha256": sha256_file(args.filesystem),
        },
        "breaking": args.breaking,
        "changelog": args.changelog,
        "notes": args.notes,
    }

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    with open(args.output, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    # Summary
    print(f"ota-manifest.json generated:")
    print(f"  version:          {args.version}")
    print(f"  min_from_version: {args.min_from_version}")
    print(f"  firmware:         {fw_name} ({file_size(args.firmware):,} bytes)")
    print(f"  filesystem:       {fs_name} ({file_size(args.filesystem):,} bytes)")
    print(f"  breaking:         {args.breaking}")
    print(f"  output:           {args.output}")


if __name__ == "__main__":
    main()
