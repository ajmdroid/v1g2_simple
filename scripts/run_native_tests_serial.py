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
NATIVE_BUILD_DIR = ROOT / ".pio" / "build" / "native"


def discover_native_tests() -> list[str]:
    return sorted(
        path.name
        for path in TEST_ROOT.iterdir()
        if path.is_dir()
        and path.name.startswith("test_")
        and not path.name.startswith("test_device_")
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "tests",
        nargs="*",
        help="Optional native test directory names (for example: test_display_pipeline_module).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    available = discover_native_tests()
    selected = args.tests or available

    unknown = sorted(set(selected) - set(available))
    if unknown:
        print("[native-serial] unknown native test suite(s):")
        for name in unknown:
            print(f"  - {name}")
        return 2

    failures: list[tuple[str, int]] = []

    for index, test_name in enumerate(selected, start=1):
        if NATIVE_BUILD_DIR.exists():
            shutil.rmtree(NATIVE_BUILD_DIR)

        print(f"[native-serial] ({index}/{len(selected)}) running {test_name}")
        result = subprocess.run(
            ["pio", "test", "-e", "native", "-f", test_name],
            cwd=ROOT,
        )
        if result.returncode != 0:
            failures.append((test_name, result.returncode))

    if failures:
        print("[native-serial] failed suite(s):")
        for test_name, returncode in failures:
            print(f"  - {test_name} (exit {returncode})")
        return 1

    print(f"[native-serial] all {len(selected)} suite(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
