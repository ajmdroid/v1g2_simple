#!/usr/bin/env python3
"""Snapshot extern/global usage across production source files.

Contract:
1) All non-test `extern` declarations/usages under `include/` and `src/`
   remain an intentional reviewed surface.
2) Any new `extern` symbol or new source-level `extern` dependency requires an
   explicit snapshot update.

Use `--update` after intentionally changing the extern/global surface.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (ROOT / "include", ROOT / "src")
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}
CONTRACT_FILE = ROOT / "test" / "contracts" / "extern_usage_contract.txt"
EXTERN_RE = re.compile(r"^\s*extern\b(?!\s*\"C\")")


def iter_source_files() -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                files.append(path)
    return sorted(files)


def scan_entries() -> list[str]:
    entries: list[str] = []
    for path in iter_source_files():
        relative = path.relative_to(ROOT).as_posix()
        kind = "header" if path.suffix in {".h", ".hpp"} else "source"
        for line_no, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            line = raw_line.strip()
            if not line or line.startswith("//"):
                continue
            if EXTERN_RE.match(raw_line):
                compact = " ".join(line.split())
                entries.append(f"kind={kind} file={relative} line={line_no} text={compact}")
    return entries


def read_expected(path: Path) -> list[str]:
    if not path.exists():
        return []
    lines: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    return lines


def write_expected(path: Path, entries: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = ["# Extern/global usage contract snapshot", ""]
    payload.extend(entries)
    payload.append("")
    path.write_text("\n".join(payload), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--update", action="store_true", help="rewrite the contract snapshot")
    args = parser.parse_args()

    actual = scan_entries()
    expected = read_expected(CONTRACT_FILE)

    if args.update:
        write_expected(CONTRACT_FILE, actual)
        header_count = sum(1 for entry in actual if entry.startswith("kind=header "))
        source_count = sum(1 for entry in actual if entry.startswith("kind=source "))
        print(
            "[contract] wrote extern-usage snapshot "
            f"({len(actual)} entries: {header_count} header, {source_count} source)"
        )
        return 0

    if actual != expected:
        print("[contract] extern/global usage drift detected.")
        diff = difflib.unified_diff(
            expected,
            actual,
            fromfile=CONTRACT_FILE.as_posix(),
            tofile="current scan",
            lineterm="",
        )
        for line in diff:
            print(line)
        print("\nRe-run with --update only if the extern/global surface change is intentional.")
        return 1

    header_count = sum(1 for entry in actual if entry.startswith("kind=header "))
    source_count = sum(1 for entry in actual if entry.startswith("kind=source "))
    print(
        "[contract] extern/global usage contract matches "
        f"({len(actual)} entries: {header_count} header, {source_count} source)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
