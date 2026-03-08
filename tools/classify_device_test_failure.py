#!/usr/bin/env python3
"""Classify device-test failures into ladder policy classes (A/B/C)."""

from __future__ import annotations

import csv
import re
import sys
from pathlib import Path


CLASS_A_SUBSTRINGS = (
    "reboot detected",
    "rebooted",
    "serial panic/wdt",
    "serial reset signatures",
    "panic endpoint reported crashes",
    "reboot evidence",
    "unstable recovery event",
    "samples-to-stable unavailable",
    "recovery time unavailable",
    "parsefailures delta",
    "oversizedrops delta",
)

CLASS_C_ALLOWED_PREFIXES = ("metrics_endpoint", "camera_smoke", "rad_short_")
CLASS_C_TRANSIENT_SUBSTRINGS = (
    "no response",
    "invalid_json",
    "timed out",
    "timeout",
    "connection reset",
    "connection refused",
    "name or service not known",
    "temporary failure",
    "cleanup stop exception",
    "exception:",
)

PARSE_FAIL_RE = re.compile(r"parsefail=([0-9]+)")


def classify_rows(rows: list[dict[str, str]]) -> tuple[str, str]:
    fail_rows = [row for row in rows if row.get("status", "").upper() == "FAIL"]
    if not fail_rows:
        return ("NONE", "No failing rows.")

    class_a_hits: list[str] = []
    for row in fail_rows:
        test_name = row.get("test", "")
        metrics = row.get("metrics", "")
        text = f"{test_name} {metrics}".lower()
        if test_name == "uptime_continuity":
            class_a_hits.append("uptime continuity reboot detection")
        parse_match = PARSE_FAIL_RE.search(text)
        if parse_match and int(parse_match.group(1)) > 0:
            class_a_hits.append(f"{test_name}: parseFail>0")
        for token in CLASS_A_SUBSTRINGS:
            if token in text:
                class_a_hits.append(f"{test_name}: contains '{token}'")

    if class_a_hits:
        reason = "; ".join(class_a_hits[:4])
        return ("A", reason)

    transient_hits = 0
    class_c_candidate = True
    for row in fail_rows:
        test_name = row.get("test", "")
        metrics = row.get("metrics", "")
        lowered = metrics.lower()
        if not test_name.startswith(CLASS_C_ALLOWED_PREFIXES):
            class_c_candidate = False
        token_hit = any(token in lowered for token in CLASS_C_TRANSIENT_SUBSTRINGS)
        if token_hit:
            transient_hits += 1
        else:
            class_c_candidate = False

    if class_c_candidate and transient_hits == len(fail_rows):
        tests = ", ".join(row.get("test", "unknown") for row in fail_rows)
        return ("C", f"Endpoint/harness transient signatures on: {tests}")

    fail_tests = ", ".join(row.get("test", "unknown") for row in fail_rows)
    return ("B", f"Performance/stability gate failures on: {fail_tests}")


def load_rows(tsv_path: Path) -> list[dict[str, str]]:
    with tsv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        return list(reader)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: classify_device_test_failure.py <device_test_dir_or_summary>", file=sys.stderr)
        return 2

    raw = Path(sys.argv[1]).expanduser()
    if raw.is_dir():
        run_dir = raw
    else:
        run_dir = raw.parent
    tsv_path = run_dir / "results.tsv"
    if not tsv_path.exists():
        print("class=C")
        print("reason=results.tsv missing (harness/artifact transient)")
        return 0

    rows = load_rows(tsv_path)
    klass, reason = classify_rows(rows)
    print(f"class={klass}")
    print(f"reason={reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
