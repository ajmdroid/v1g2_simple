#!/usr/bin/env python3
"""
Score perf CSV captures against docs/PERF_SLOS.md.

Usage:
  python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv --profile drive_wifi_off
  python tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1.csv --profile drive_wifi_off --json

Exit codes:
  0 = hard SLO pass (advisories may warn)
  1 = hard SLO pass, advisory failures present
  2 = hard SLO failure
  3 = tool/input error
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


PROFILES = ("drive_wifi_off", "drive_wifi_ap")


HARD_COMMON = [
    ("qDrop", "final", "==", 0.0),
    ("parseFail", "final", "==", 0.0),
    ("oversizeDrops", "final", "==", 0.0),
    ("bleMutexTimeout", "final", "==", 0.0),
    ("loopMax_us", "max", "<=", 250000.0),
    ("bleDrainMax_us", "max", "<=", 10000.0),
    ("bleProcessMax_us", "max", "<=", 120000.0),
    ("dispPipeMax_us", "max", "<=", 120000.0),
    ("flushMax_us", "max", "<=", 100000.0),
    ("sdMax_us", "max", "<=", 50000.0),
    ("fsMax_us", "max", "<=", 50000.0),
    ("queueHighWater", "final", "<=", 12.0),
    ("dmaLargestMin", "min", ">=", 10000.0),
    ("dmaFreeMin", "min", ">=", 20000.0),
    ("cameraLoadFailures", "final", "==", 0.0),
    ("cameraBudgetExceeded", "final", "==", 0.0),
    ("cameraIndexSwapFailures", "final", "==", 0.0),
    ("cameraMaxTick_us", "max", "<=", 800.0),
]

HARD_PROFILE = {
    "drive_wifi_off": [
        ("wifiConnectDeferred", "final", "==", 0.0),
        ("wifiMax_us", "max", "<=", 1000.0),
    ],
    "drive_wifi_ap": [
        ("wifiConnectDeferred", "final", "<=", 5.0),
        ("wifiMax_us", "max", "<=", 5000.0),
    ],
}

ADVISORY = [
    ("cmdPaceNotYetPerMin", "computed", "<=", 25.0),
    ("displaySkipPct", "computed", "<=", 20.0),
    ("displaySkipsPerMin", "computed", "<=", 120.0),
    ("gpsObsDropsPerMin", "computed", "<=", 200.0),
    ("audioPlayBusyPerMin", "computed", "<=", 2.0),
    ("reconn", "final", "<=", 2.0),
    ("disc", "final", "<=", 2.0),
    ("cameraSkipNonCorePct", "computed", "<=", 98.0),
]

HARD_COMPUTED = [
    ("cameraMaxWindowHz", "computed", "<=", 5.05),
]


@dataclass
class CheckResult:
    metric: str
    level: str
    op: str
    limit: float
    value: float
    passed: bool
    source: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Score perf CSV against Perf SLOs")
    parser.add_argument("csv_path", help="Path to perf_boot_*.csv")
    parser.add_argument("--profile", choices=PROFILES, default="drive_wifi_off")
    parser.add_argument("--json", action="store_true", help="Output JSON")
    return parser.parse_args()


def parse_int(value: str) -> int:
    value = (value or "").strip()
    if value == "":
        return 0
    try:
        return int(value)
    except ValueError:
        # Accept accidental float-like values without crashing.
        return int(float(value))


def load_rows(path: Path) -> List[Dict[str, int]]:
    rows: List[Dict[str, int]] = []
    with path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        if not reader.fieldnames:
            raise RuntimeError("CSV has no header")
        for row in reader:
            millis = (row.get("millis") or "").strip()
            if not millis or millis.startswith("#"):
                continue
            parsed: Dict[str, int] = {}
            for key, value in row.items():
                parsed[key] = parse_int(value or "0")
            rows.append(parsed)
    if not rows:
        raise RuntimeError("No data rows found in CSV")
    return rows


def compare(value: float, op: str, limit: float) -> bool:
    if op == "==":
        return math.isclose(value, limit, rel_tol=0.0, abs_tol=1e-9)
    if op == "<=":
        return value <= limit
    if op == ">=":
        return value >= limit
    raise ValueError(f"Unsupported op: {op}")


def max_of(rows: List[Dict[str, int]], field: str) -> float:
    return float(max(r[field] for r in rows))


def min_of(rows: List[Dict[str, int]], field: str) -> float:
    return float(min(r[field] for r in rows))


def final_of(rows: List[Dict[str, int]], field: str) -> float:
    return float(rows[-1][field])


def duration_s(rows: List[Dict[str, int]]) -> float:
    millis = rows[-1]["millis"]
    return max(0.001, millis / 1000.0)


def camera_max_window_hz(rows: List[Dict[str, int]]) -> float:
    peak = 0.0
    for i in range(1, len(rows)):
        prev = rows[i - 1]
        cur = rows[i]
        dt = (cur["millis"] - prev["millis"]) / 1000.0
        if dt <= 0:
            continue
        tick_inc = cur["cameraTicks"] - prev["cameraTicks"]
        hz = tick_inc / dt
        if hz > peak:
            peak = hz
    return peak


def compute_value(rows: List[Dict[str, int]], metric: str) -> float:
    dur = duration_s(rows)
    if metric == "cameraMaxWindowHz":
        return camera_max_window_hz(rows)
    if metric == "cmdPaceNotYetPerMin":
        return final_of(rows, "cmdPaceNotYet") * 60.0 / dur
    if metric == "displaySkipPct":
        updates = final_of(rows, "displayUpdates")
        skips = final_of(rows, "displaySkips")
        total = updates + skips
        if total <= 0:
            return 0.0
        return skips * 100.0 / total
    if metric == "displaySkipsPerMin":
        return final_of(rows, "displaySkips") * 60.0 / dur
    if metric == "gpsObsDropsPerMin":
        return final_of(rows, "gpsObsDrops") * 60.0 / dur
    if metric == "audioPlayBusyPerMin":
        return final_of(rows, "audioPlayBusy") * 60.0 / dur
    if metric == "cameraSkipNonCorePct":
        ticks = final_of(rows, "cameraTicks")
        skips = final_of(rows, "cameraTickSkipsNonCore")
        total = ticks + skips
        if total <= 0:
            return 0.0
        return (skips * 100.0) / total
    raise KeyError(f"Unknown computed metric: {metric}")


def run_check(
    rows: List[Dict[str, int]],
    metric: str,
    source: str,
    op: str,
    limit: float,
    level: str,
) -> CheckResult:
    if source == "max":
        value = max_of(rows, metric)
    elif source == "min":
        value = min_of(rows, metric)
    elif source == "final":
        value = final_of(rows, metric)
    elif source == "computed":
        value = compute_value(rows, metric)
    else:
        raise ValueError(f"Unsupported source: {source}")

    passed = compare(value, op, limit)
    return CheckResult(
        metric=metric,
        level=level,
        op=op,
        limit=limit,
        value=value,
        passed=passed,
        source=source,
    )


def evaluate(rows: List[Dict[str, int]], profile: str) -> List[CheckResult]:
    checks: List[CheckResult] = []
    for metric, source, op, limit in HARD_COMMON:
        checks.append(run_check(rows, metric, source, op, limit, "hard"))
    for metric, source, op, limit in HARD_COMPUTED:
        checks.append(run_check(rows, metric, source, op, limit, "hard"))
    for metric, source, op, limit in HARD_PROFILE[profile]:
        checks.append(run_check(rows, metric, source, op, limit, "hard"))
    for metric, source, op, limit in ADVISORY:
        checks.append(run_check(rows, metric, source, op, limit, "advisory"))
    return checks


def format_value(metric: str, value: float) -> str:
    if metric.endswith("PerMin") or metric.endswith("Pct") or metric.endswith("Hz"):
        return f"{value:.2f}"
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return f"{value:.2f}"


def print_human(path: Path, profile: str, rows: List[Dict[str, int]], checks: List[CheckResult]) -> None:
    dur = duration_s(rows)
    hard = [c for c in checks if c.level == "hard"]
    adv = [c for c in checks if c.level == "advisory"]
    hard_fail = [c for c in hard if not c.passed]
    adv_fail = [c for c in adv if not c.passed]

    print("=" * 72)
    print("PERF CSV SLO SCORECARD")
    print("=" * 72)
    print(f"File: {path}")
    print(f"Profile: {profile}")
    print(f"Rows: {len(rows)}")
    print(f"Duration: {dur:.2f}s")
    display_updates = int(final_of(rows, "displayUpdates"))
    display_skips = int(final_of(rows, "displaySkips"))
    display_skip_pct = compute_value(rows, "displaySkipPct")
    print(
        "Display: updates="
        f"{display_updates}, skips={display_skips}, skipPct={display_skip_pct:.2f}%"
    )
    print()
    print("Hard SLOs:")
    for c in hard:
        status = "PASS" if c.passed else "FAIL"
        print(
            f"  [{status}] {c.metric:22s} value={format_value(c.metric, c.value):>10s} "
            f"{c.op} {format_value(c.metric, c.limit):>10s} ({c.source})"
        )
    print()
    print("Advisory SLOs:")
    for c in adv:
        status = "PASS" if c.passed else "WARN"
        print(
            f"  [{status}] {c.metric:22s} value={format_value(c.metric, c.value):>10s} "
            f"{c.op} {format_value(c.metric, c.limit):>10s} ({c.source})"
        )
    print()
    print("-" * 72)
    print(f"Hard failures: {len(hard_fail)}")
    print(f"Advisory warnings: {len(adv_fail)}")
    if hard_fail:
        print("Result: FAIL")
    elif adv_fail:
        print("Result: PASS_WITH_WARNINGS")
    else:
        print("Result: PASS")


def main() -> int:
    args = parse_args()
    path = Path(args.csv_path)
    if not path.exists():
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        return 3

    try:
        rows = load_rows(path)
        checks = evaluate(rows, args.profile)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    hard_fail = [c for c in checks if c.level == "hard" and not c.passed]
    adv_fail = [c for c in checks if c.level == "advisory" and not c.passed]

    if args.json:
        payload = {
            "file": str(path),
            "profile": args.profile,
            "rows": len(rows),
            "duration_s": duration_s(rows),
            "hard_failures": len(hard_fail),
            "advisory_warnings": len(adv_fail),
            "checks": [
                {
                    "metric": c.metric,
                    "level": c.level,
                    "source": c.source,
                    "value": c.value,
                    "op": c.op,
                    "limit": c.limit,
                    "passed": c.passed,
                }
                for c in checks
            ],
        }
        print(json.dumps(payload, indent=2))
    else:
        print_human(path, args.profile, rows, checks)

    if hard_fail:
        return 2
    if adv_fail:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
