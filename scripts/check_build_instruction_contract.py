#!/usr/bin/env python3
"""Validate tracked build docs stay aligned with build.sh usage."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOC_PATHS = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md"))]


def main() -> int:
    errors: list[str] = []
    usage_pattern = re.compile(r"\./build\.sh\b[^\n]*\s-n(\s|$)")

    for path in DOC_PATHS:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if usage_pattern.search(text):
            errors.append(f"{path.relative_to(ROOT)} mentions unsupported build.sh -n flag")

    if errors:
        print("[contract] build-docs mismatch:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] tracked build docs match supported build.sh usage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
