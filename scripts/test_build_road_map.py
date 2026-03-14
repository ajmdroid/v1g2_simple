#!/usr/bin/env python3
"""Regression tests for scripts/build_road_map.py."""

from __future__ import annotations

import importlib.util
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = ROOT / "scripts" / "build_road_map.py"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_builder_module():
    spec = importlib.util.spec_from_file_location("build_road_map", SCRIPT_PATH)
    assert_true(spec is not None and spec.loader is not None, "failed to load build_road_map module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_help_no_longer_exposes_camera_flags() -> None:
    result = subprocess.run(
        [sys.executable, str(SCRIPT_PATH), "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(result.returncode == 0, f"--help failed: {result.stderr}")
    assert_true("--cameras" not in result.stdout, "camera CLI flag should be removed")
    assert_true("camera overlay" not in result.stdout.lower(), "camera help text should be removed")


def test_write_bin_keeps_legacy_header_fields_zero(tmpdir: Path) -> None:
    builder = load_builder_module()
    segment = builder.Segment(
        builder.RC_MOTORWAY,
        False,
        [(39.7500, -104.9900), (39.7510, -104.9890)],
        65,
    )
    output_path = tmpdir / "road_map.bin"

    stats = builder.write_bin([segment], output_path, 0.5, 100)
    assert_true(stats is not None, "write_bin should return stats")

    data = output_path.read_bytes()
    assert_true(len(data) >= builder.HEADER_SIZE, "road_map.bin should contain a full header")
    reserved_offset, reserved_count = struct.unpack_from("<II", data, 56)
    assert_true(reserved_offset == 0, f"legacy reserved offset must remain zero, got {reserved_offset}")
    assert_true(reserved_count == 0, f"legacy reserved count must remain zero, got {reserved_count}")


def main() -> int:
    test_help_no_longer_exposes_camera_flags()
    with tempfile.TemporaryDirectory(prefix="build_road_map_") as tmp:
        test_write_bin_keeps_legacy_header_fields_zero(Path(tmp))
    print("[build-road-map] regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
