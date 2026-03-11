#!/usr/bin/env python3
"""Derive data-driven SLO thresholds from observed perf CSV artifacts.

Reads all perf CSVs under .artifacts/, computes the P99 of per-file
max values for each latency metric, applies a headroom multiplier,
and outputs proposed thresholds.

Usage:
    python tools/derive_slo_thresholds.py                         # dry-run
    python tools/derive_slo_thresholds.py --apply                 # update JSON
    python tools/derive_slo_thresholds.py --headroom 1.3          # 30% headroom
    python tools/derive_slo_thresholds.py --artifacts-dir /path   # custom dir
"""

import argparse
import csv
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARTIFACTS = ROOT / ".artifacts"
DEFAULT_SLO_FILE = ROOT / "tools" / "perf_slo_thresholds.json"

# Latency metrics where source=max and op=<=
LATENCY_METRICS = [
    "loopMax_us",
    "bleDrainMax_us",
    "bleProcessMax_us",
    "dispPipeMax_us",
    "flushMax_us",
    "sdMax_us",
    "fsMax_us",
]

# Memory metrics where source=min and op=>=
MEMORY_METRICS = [
    "dmaLargestMin",
    "dmaFreeMin",
]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--artifacts-dir", default=str(DEFAULT_ARTIFACTS), help="Directory containing perf CSVs")
    p.add_argument("--slo-file", default=str(DEFAULT_SLO_FILE), help="Path to perf_slo_thresholds.json")
    p.add_argument("--headroom", type=float, default=1.2, help="Multiplier for headroom (default: 1.2 = 20%%)")
    p.add_argument("--apply", action="store_true", help="Update the SLO JSON file in place")
    p.add_argument("--json", action="store_true", help="Output results as JSON")
    return p.parse_args()


def find_csvs(artifacts_dir: Path):
    return sorted(artifacts_dir.rglob("perf_boot_*.csv"))


def load_csv_rows(path: Path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = []
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            lines.append(stripped)
        if len(lines) < 2:
            return rows
        reader = csv.DictReader(lines)
        for row in reader:
            parsed = {}
            for k, v in row.items():
                if k is None:
                    continue
                try:
                    parsed[k] = int(v)
                except (ValueError, TypeError):
                    try:
                        parsed[k] = int(float(v))
                    except (ValueError, TypeError):
                        parsed[k] = 0
            rows.append(parsed)
    return rows


def percentile(values, pct):
    if not values:
        return 0
    s = sorted(values)
    idx = (pct / 100.0) * (len(s) - 1)
    lo = int(math.floor(idx))
    hi = int(math.ceil(idx))
    if lo == hi:
        return s[lo]
    frac = idx - lo
    return s[lo] * (1 - frac) + s[hi] * frac


def round_to(value, step):
    return int(math.ceil(value / step)) * step


def derive(artifacts_dir: Path, headroom: float):
    csvs = find_csvs(artifacts_dir)
    if not csvs:
        print(f"No perf CSVs found in {artifacts_dir}", file=sys.stderr)
        sys.exit(1)

    # Collect per-file aggregates
    latency_maxes = {m: [] for m in LATENCY_METRICS}
    memory_mins = {m: [] for m in MEMORY_METRICS}

    skipped = 0
    for csv_path in csvs:
        rows = load_csv_rows(csv_path)
        if not rows:
            skipped += 1
            continue

        for metric in LATENCY_METRICS:
            values = [r.get(metric, 0) for r in rows if metric in r]
            if values:
                latency_maxes[metric].append(max(values))

        for metric in MEMORY_METRICS:
            values = [r.get(metric, 0) for r in rows if metric in r]
            if values:
                memory_mins[metric].append(min(values))

    results = {}

    for metric in LATENCY_METRICS:
        observations = latency_maxes[metric]
        if not observations:
            continue
        p99 = percentile(observations, 99)
        proposed = round_to(p99 * headroom, 1000)
        results[metric] = {
            "files": len(observations),
            "p50": int(percentile(observations, 50)),
            "p95": int(percentile(observations, 95)),
            "p99": int(p99),
            "observed_max": int(max(observations)),
            "proposed_limit": proposed,
            "op": "<=",
            "source": "max",
        }

    for metric in MEMORY_METRICS:
        observations = memory_mins[metric]
        if not observations:
            continue
        # For >=, use the 1st percentile (worst case) with inverse headroom
        p1 = percentile(observations, 1)
        proposed = round_to(p1 / headroom, 1000)
        results[metric] = {
            "files": len(observations),
            "p50": int(percentile(observations, 50)),
            "p5": int(percentile(observations, 5)),
            "p1": int(p1),
            "observed_min": int(min(observations)),
            "proposed_limit": proposed,
            "op": ">=",
            "source": "min",
        }

    return results, len(csvs), skipped


def apply_to_slo_file(results, slo_path: Path):
    payload = json.loads(slo_path.read_text(encoding="utf-8"))

    updated = 0
    for entry in payload.get("hard_common", []):
        metric = entry.get("metric")
        if metric in results:
            old = entry["limit"]
            new = results[metric]["proposed_limit"]
            if old != new:
                entry["limit"] = new
                updated += 1

    slo_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return updated


def main():
    args = parse_args()
    artifacts_dir = Path(args.artifacts_dir)
    slo_path = Path(args.slo_file)

    results, total_csvs, skipped = derive(artifacts_dir, args.headroom)

    if args.json:
        print(json.dumps({"total_csvs": total_csvs, "skipped": skipped, "metrics": results}, indent=2))
        return

    print(f"Analyzed {total_csvs} CSVs ({skipped} skipped/empty)")
    print(f"Headroom multiplier: {args.headroom}x\n")

    # Load current thresholds for comparison
    current = {}
    if slo_path.exists():
        payload = json.loads(slo_path.read_text(encoding="utf-8"))
        for entry in payload.get("hard_common", []):
            current[entry["metric"]] = entry["limit"]

    print(f"{'Metric':<22} {'P50':>10} {'P95':>10} {'P99':>10} {'ObsMax':>10} {'Current':>10} {'Proposed':>10} {'Delta':>8}")
    print("-" * 94)

    for metric in LATENCY_METRICS:
        if metric not in results:
            continue
        r = results[metric]
        cur = current.get(metric, "—")
        delta = ""
        if isinstance(cur, (int, float)):
            diff = r["proposed_limit"] - cur
            delta = f"{diff:+d}"
        print(f"{metric:<22} {r['p50']:>10,} {r['p95']:>10,} {r['p99']:>10,} {r['observed_max']:>10,} {str(cur):>10} {r['proposed_limit']:>10,} {delta:>8}")

    print()
    for metric in MEMORY_METRICS:
        if metric not in results:
            continue
        r = results[metric]
        cur = current.get(metric, "—")
        delta = ""
        if isinstance(cur, (int, float)):
            diff = r["proposed_limit"] - cur
            delta = f"{diff:+d}"
        print(f"{metric:<22} {r['p50']:>10,} {r.get('p5', '—'):>10} {r.get('p1', '—'):>10} {r['observed_min']:>10,} {str(cur):>10} {r['proposed_limit']:>10,} {delta:>8}")

    if args.apply:
        count = apply_to_slo_file(results, slo_path)
        print(f"\nUpdated {count} threshold(s) in {slo_path}")
    else:
        print(f"\nDry run. Use --apply to update {slo_path}")


if __name__ == "__main__":
    main()
