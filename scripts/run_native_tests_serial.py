#!/usr/bin/env python3
"""Run PlatformIO native test suites one at a time with a clean build dir.

This avoids aggregate `pio test -e native` cross-suite artifact reuse, which can
surface stale binaries, missing `program` artifacts, or sporadic SIGKILLs that
do not reproduce when the same suite is run in isolation.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = ROOT / "test"

VALID_ENVS = {"native", "native-replay", "native-sanitized"}


def discover_native_tests(env: str) -> list[str]:
    ignore_prefix = "test_device_"
    # native-replay only runs test_drive_replay
    if env == "native-replay":
        replay_dir = TEST_ROOT / "test_drive_replay"
        return ["test_drive_replay"] if replay_dir.is_dir() else []
    return sorted(
        path.name
        for path in TEST_ROOT.iterdir()
        if path.is_dir()
        and path.name.startswith("test_")
        and not path.name.startswith(ignore_prefix)
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--env",
        default="native",
        choices=sorted(VALID_ENVS),
        help="PlatformIO test environment (default: native).",
    )
    parser.add_argument(
        "tests",
        nargs="*",
        help="Optional native test directory names (for example: test_display_pipeline_module).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    env = args.env
    build_dir = ROOT / ".pio" / "build" / env
    available = discover_native_tests(env)
    selected = args.tests or available

    unknown = sorted(set(selected) - set(available))
    if unknown:
        print(f"[{env}-serial] unknown test suite(s):")
        for name in unknown:
            print(f"  - {name}")
        return 2

    failures: list[tuple[str, int]] = []

    for index, test_name in enumerate(selected, start=1):
        if build_dir.exists():
            shutil.rmtree(build_dir)

        print(f"[{env}-serial] ({index}/{len(selected)}) running {test_name}")
        result = subprocess.run(
            ["pio", "test", "-e", env, "-f", test_name],
            cwd=ROOT,
        )
        if result.returncode != 0:
            failures.append((test_name, result.returncode))

    if failures:
        print(f"[{env}-serial] failed suite(s):")
        for test_name, returncode in failures:
            print(f"  - {test_name} (exit {returncode})")
        return 1

    print(f"[{env}-serial] all {len(selected)} suite(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
