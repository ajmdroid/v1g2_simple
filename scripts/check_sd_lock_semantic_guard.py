#!/usr/bin/env python3
"""Check SD lock semantic rules without snapshot coupling."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_sd_lock_discipline_contract as contract  # type: ignore  # noqa: E402


def main() -> int:
    paths = contract.scan_files()
    usage_lines = contract.extract_lock_usage_lines(paths)
    raw_mutex_violations = contract.extract_raw_sd_mutex_violations(paths)
    main_blocking_line = "file=src/main.cpp class=SDLockBlocking"
    main_uses_blocking = any(line.startswith(main_blocking_line) for line in usage_lines)

    if raw_mutex_violations or main_uses_blocking:
        if raw_mutex_violations:
            print("[guard] raw SD mutex usage violations detected")
            for row in raw_mutex_violations:
                print(f"  - {row}")
        if main_uses_blocking:
            print("[guard] src/main.cpp must not use SDLockBlocking")
        return 1

    print("[guard] SD lock semantic guard matches (0 raw mutex violations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
