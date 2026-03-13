#!/usr/bin/env python3
"""Validate local AI instructions stay aligned with build.sh usage."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INSTRUCTIONS_PATH = ROOT / ".github" / "instructions" / "v1_simple.instructions.md"


def main() -> int:
    if not INSTRUCTIONS_PATH.exists():
        print(f"[contract] build-instructions: missing file {INSTRUCTIONS_PATH}")
        return 1

    text = INSTRUCTIONS_PATH.read_text(encoding="utf-8")
    errors: list[str] = []

    if re.search(r"\./build\.sh\b[^\n]*\s-n(\s|$)", text):
        errors.append("instructions still mention unsupported build.sh -n flag")

    commit_rule = "commit after every change, with a clear message describing the change."
    commit_rule_count = text.lower().count(commit_rule)
    if commit_rule_count > 1:
        errors.append(f"duplicate commit rule appears {commit_rule_count} times")

    if errors:
        print("[contract] build-instructions mismatch:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("[contract] build-instructions match supported build.sh usage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
