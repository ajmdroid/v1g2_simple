#!/usr/bin/env python3
"""Check SD lock discipline contract.

Enforced invariants:
1) Lock class usage by file stays aligned with expected snapshot.
2) No raw xSemaphoreTake(sdMutex/getSDMutex()) outside storage manager internals.
3) src/main.cpp does not use SDLockBlocking.

Use --update to rewrite expected lock-usage snapshot.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import DefaultDict, List

ROOT = Path(__file__).resolve().parents[1]
CONTRACT_FILE = ROOT / "test" / "contracts" / "sd_lock_discipline_contract.txt"

SCAN_ROOTS = (ROOT / "src", ROOT / "include")
ALLOWED_RAW_SD_MUTEX_FILES = {
    ROOT / "src" / "storage_manager.cpp",
    ROOT / "src" / "storage_manager.h",
}

LOCK_CLASS_RE = re.compile(r"\bStorageManager::(SDTryLock|SDLockBlocking|SDLockBootRetry)\b")
RAW_SD_MUTEX_RE = re.compile(
    r"\bxSemaphoreTake\s*\(\s*[^,)]*(?:sdMutex\b|getSDMutex\s*\()",
    re.DOTALL,
)
MASK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)

LOCK_CLASSES = ("SDTryLock", "SDLockBlocking", "SDLockBootRetry")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def to_relative(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def line_for_index(source: str, index: int) -> int:
    return source.count("\n", 0, index) + 1


def mask_comments_and_strings(source: str) -> str:
    def _mask(match: re.Match[str]) -> str:
        return "".join("\n" if ch == "\n" else " " for ch in match.group(0))

    return MASK_RE.sub(_mask, source)


def scan_files() -> List[Path]:
    files: List[Path] = []
    for root in SCAN_ROOTS:
        if not root.exists():
            continue
        files.extend(root.rglob("*.cpp"))
        files.extend(root.rglob("*.h"))
    deduped: List[Path] = []
    seen = set()
    for path in sorted(files):
        if path in seen:
            continue
        seen.add(path)
        deduped.append(path)
    return deduped


def extract_lock_usage_lines(paths: List[Path]) -> List[str]:
    counts: DefaultDict[Path, DefaultDict[str, int]] = defaultdict(lambda: defaultdict(int))
    for path in paths:
        source = read_text(path)
        masked = mask_comments_and_strings(source)
        for match in LOCK_CLASS_RE.finditer(masked):
            cls = match.group(1)
            counts[path][cls] += 1

    lines: List[str] = []
    for path in sorted(counts):
        for cls in LOCK_CLASSES:
            n = counts[path].get(cls, 0)
            if n == 0:
                continue
            lines.append(f"file={to_relative(path)} class={cls} count={n}")
    return lines


def extract_raw_sd_mutex_violations(paths: List[Path]) -> List[str]:
    violations: List[str] = []
    for path in paths:
        source = read_text(path)
        masked = mask_comments_and_strings(source)
        for match in RAW_SD_MUTEX_RE.finditer(masked):
            if path in ALLOWED_RAW_SD_MUTEX_FILES:
                continue
            line = line_for_index(source, match.start())
            violations.append(
                f"file={to_relative(path)} line={line} rule=raw_sd_mutex_take"
            )
    return sorted(set(violations))


def read_expected_lines(path: Path) -> List[str]:
    if not path.exists():
        return []
    lines: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    return lines


def write_lines(path: Path, header: str, lines: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = [header, ""]
    payload.extend(lines)
    payload.append("")
    path.write_text("\n".join(payload), encoding="utf-8")


def print_diff(expected: List[str], actual: List[str]) -> None:
    expected_set = set(expected)
    actual_set = set(actual)
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)

    print("[contract] sd-lock-discipline snapshot mismatch")
    if missing:
        print("  missing:")
        for row in missing:
            print(f"    - {row}")
    if extra:
        print("  extra:")
        for row in extra:
            print(f"    + {row}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract snapshot from current source",
    )
    args = parser.parse_args()

    paths = scan_files()
    usage_lines = extract_lock_usage_lines(paths)
    raw_mutex_violations = extract_raw_sd_mutex_violations(paths)

    main_blocking_line = "file=src/main.cpp class=SDLockBlocking"
    main_uses_blocking = any(line.startswith(main_blocking_line) for line in usage_lines)

    if args.update:
        write_lines(
            CONTRACT_FILE,
            "# SD lock discipline contract (lock class usage by file)",
            usage_lines,
        )
        print(f"Updated {CONTRACT_FILE}")

    expected_usage = read_expected_lines(CONTRACT_FILE)
    ok = True

    if expected_usage != usage_lines:
        print_diff(expected_usage, usage_lines)
        ok = False

    if raw_mutex_violations:
        print("[contract] raw SD mutex usage violations detected")
        for row in raw_mutex_violations:
            print(f"  - {row}")
        ok = False

    if main_uses_blocking:
        print("[contract] src/main.cpp must not use SDLockBlocking")
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print(
        "[contract] sd-lock-discipline contract matches "
        f"({len(usage_lines)} lock-usage entries, 0 raw mutex violations)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
