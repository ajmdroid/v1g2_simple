#!/usr/bin/env python3
"""
Runtime smoke verification for perf counters.

Workflow:
1) Read baseline values from /api/debug/metrics.
2) For each target counter, prompt the operator to trigger the event.
3) Poll /api/debug/metrics until the counter increments.
4) Verify an SD perf CSV row eventually reflects the observed values.

Designed to be surgical and dependency-free (stdlib only).
"""

from __future__ import annotations

import argparse
import csv
import json
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


PROFILE_COUNTERS: Dict[str, List[str]] = {
    # Non-destructive path for regular operator checks.
    "power_safe": [
        "powerAutoPowerArmed",
        "powerAutoPowerTimerStart",
        "powerAutoPowerTimerCancel",
        "powerCriticalWarn",
    ],
    # Includes shutdown-producing counters.
    "power_full": [
        "powerAutoPowerArmed",
        "powerAutoPowerTimerStart",
        "powerAutoPowerTimerCancel",
        "powerAutoPowerTimerExpire",
        "powerCriticalWarn",
        "powerCriticalShutdown",
    ],
}

COUNTER_PROMPTS: Dict[str, str] = {
    "powerAutoPowerArmed": "Ensure V1 data is flowing at least once (arms auto power-off).",
    "powerAutoPowerTimerStart": "Disconnect V1 while armed and auto power-off is enabled (>0 min).",
    "powerAutoPowerTimerCancel": "Reconnect V1 before timeout to cancel timer.",
    "powerAutoPowerTimerExpire": "Disconnect V1 and wait until auto power-off timeout expires (device will power off).",
    "powerCriticalWarn": "Lower battery to critical threshold on battery power to trigger warning.",
    "powerCriticalShutdown": "Keep battery critical for >5 seconds until shutdown triggers (device will power off).",
}

DESTRUCTIVE_COUNTERS = {
    "powerAutoPowerTimerExpire",
    "powerCriticalShutdown",
}


@dataclass
class CounterResult:
    name: str
    start_value: int
    end_value: int
    elapsed_s: float
    ok: bool
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Runtime smoke verification for perf counters")
    parser.add_argument("--base-url", default="http://192.168.35.1", help="Device base URL")
    parser.add_argument(
        "--profile",
        choices=sorted(PROFILE_COUNTERS.keys()),
        default="power_safe",
        help="Counter profile to verify (default: power_safe)",
    )
    parser.add_argument(
        "--counters",
        default="",
        help="Comma-separated counters; overrides --profile when provided",
    )
    parser.add_argument(
        "--timeout-s",
        type=float,
        default=60.0,
        help="Per-counter wait timeout in seconds",
    )
    parser.add_argument(
        "--poll-interval-s",
        type=float,
        default=1.0,
        help="Poll interval while waiting for counter increments",
    )
    parser.add_argument(
        "--http-timeout-s",
        type=float,
        default=5.0,
        help="Per-request HTTP timeout in seconds",
    )
    parser.add_argument(
        "--csv-timeout-s",
        type=float,
        default=45.0,
        help="Timeout for CSV reflection verification",
    )
    parser.add_argument(
        "--no-csv-check",
        action="store_true",
        help="Skip CSV verification phase",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="Non-interactive mode (no Enter prompts)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print poll progress",
    )
    return parser.parse_args()


def normalize_base_url(url: str) -> str:
    return url.rstrip("/")


def http_get(base_url: str, path: str, timeout_s: float) -> str:
    req = urllib.request.Request(f"{base_url}{path}")
    with urllib.request.urlopen(req, timeout=timeout_s) as resp:
        return resp.read().decode("utf-8", errors="replace")


def http_get_json(base_url: str, path: str, timeout_s: float) -> Dict[str, object]:
    text = http_get(base_url, path, timeout_s)
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid JSON from {path}: {exc}") from exc
    if not isinstance(parsed, dict):
        raise RuntimeError(f"Unexpected JSON shape from {path}: expected object")
    return parsed


def metric_as_int(metrics: Dict[str, object], key: str) -> Optional[int]:
    value = metrics.get(key)
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def fetch_metrics(base_url: str, timeout_s: float) -> Dict[str, object]:
    return http_get_json(base_url, "/api/debug/metrics", timeout_s)


def split_counters(args: argparse.Namespace) -> List[str]:
    if args.counters.strip():
        values = [x.strip() for x in args.counters.split(",")]
        return [x for x in values if x]
    return list(PROFILE_COUNTERS[args.profile])


def wait_for_increment(
    base_url: str,
    counter: str,
    start_value: int,
    timeout_s: float,
    poll_interval_s: float,
    http_timeout_s: float,
    verbose: bool,
) -> CounterResult:
    started = time.time()
    last_val = start_value
    while True:
        elapsed = time.time() - started
        if elapsed > timeout_s:
            return CounterResult(
                name=counter,
                start_value=start_value,
                end_value=last_val,
                elapsed_s=elapsed,
                ok=False,
                message="timeout waiting for increment",
            )
        try:
            metrics = fetch_metrics(base_url, http_timeout_s)
            maybe_val = metric_as_int(metrics, counter)
        except Exception as exc:  # noqa: BLE001 - operator script; keep running through transient errors
            maybe_val = None
            if verbose:
                print(f"  [poll] {counter}: request failed ({exc})")

        if maybe_val is not None:
            last_val = maybe_val
            if maybe_val > start_value:
                return CounterResult(
                    name=counter,
                    start_value=start_value,
                    end_value=maybe_val,
                    elapsed_s=elapsed,
                    ok=True,
                    message=f"incremented by +{maybe_val - start_value}",
                )
            if verbose:
                print(f"  [poll] {counter}: {maybe_val} (waiting for > {start_value})")
        elif verbose:
            print(f"  [poll] {counter}: missing/non-integer value")

        time.sleep(max(0.05, poll_interval_s))


def choose_perf_file(base_url: str, timeout_s: float) -> str:
    payload = http_get_json(base_url, "/api/debug/perf-files", timeout_s)
    files_obj = payload.get("files")
    if not isinstance(files_obj, list) or not files_obj:
        raise RuntimeError("No perf CSV files returned by /api/debug/perf-files")

    active_name: Optional[str] = None
    fallback_name: Optional[str] = None
    fallback_boot = -1

    for item in files_obj:
        if not isinstance(item, dict):
            continue
        name = item.get("name")
        if not isinstance(name, str):
            continue
        boot_id = int(item.get("bootId", 0))
        if fallback_name is None or boot_id > fallback_boot:
            fallback_name = name
            fallback_boot = boot_id
        if bool(item.get("active", False)):
            active_name = name

    chosen = active_name or fallback_name
    if not chosen:
        raise RuntimeError("No valid perf CSV file name found in /api/debug/perf-files")
    return chosen


def download_perf_csv(base_url: str, name: str, timeout_s: float) -> str:
    query = urllib.parse.urlencode({"name": name})
    path = f"/api/debug/perf-files/download?{query}"
    return http_get(base_url, path, timeout_s)


def parse_csv_snapshot(csv_text: str) -> Tuple[Optional[str], List[str], Dict[str, int]]:
    lines = [ln for ln in csv_text.splitlines() if ln.strip()]
    marker_line = next((ln for ln in lines if ln.startswith("#session_start")), None)
    data_lines = [ln for ln in lines if not ln.startswith("#")]
    if not data_lines:
        return marker_line, [], {}

    reader = csv.reader(data_lines)
    try:
        header = next(reader)
    except StopIteration:
        return marker_line, [], {}

    last_row: Optional[List[str]] = None
    for row in reader:
        if row:
            last_row = row

    if not last_row:
        return marker_line, header, {}

    values: Dict[str, int] = {}
    if len(last_row) == len(header):
        for i, key in enumerate(header):
            try:
                values[key] = int(last_row[i])
            except (TypeError, ValueError):
                continue
    return marker_line, header, values


def wait_for_csv_reflection(
    base_url: str,
    csv_name: str,
    required_values: Dict[str, int],
    timeout_s: float,
    poll_interval_s: float,
    http_timeout_s: float,
    verbose: bool,
) -> Tuple[bool, str]:
    started = time.time()
    while True:
        elapsed = time.time() - started
        if elapsed > timeout_s:
            return False, "timeout waiting for CSV to reflect expected values"

        try:
            csv_text = download_perf_csv(base_url, csv_name, http_timeout_s)
            marker, header, row_values = parse_csv_snapshot(csv_text)
        except Exception as exc:  # noqa: BLE001 - operator script; keep trying
            if verbose:
                print(f"  [csv] download failed: {exc}")
            time.sleep(max(0.05, poll_interval_s))
            continue

        if not header:
            if verbose:
                print("  [csv] waiting for header/data rows")
            time.sleep(max(0.05, poll_interval_s))
            continue

        missing_cols = [k for k in required_values.keys() if k not in header]
        if missing_cols:
            return False, f"CSV missing columns: {', '.join(missing_cols)}"

        if marker is None:
            return False, "CSV missing #session_start marker"
        if "schema=" not in marker:
            return False, "session marker missing schema=<version>"

        lagging = []
        for key, min_value in required_values.items():
            current = row_values.get(key)
            if current is None or current < min_value:
                lagging.append((key, current, min_value))
        if not lagging:
            return True, "CSV row reflects expected counter values"

        if verbose:
            detail = ", ".join(f"{k}:{cur}->{target}" for k, cur, target in lagging)
            print(f"  [csv] waiting for counters to catch up: {detail}")
        time.sleep(max(0.05, poll_interval_s))


def prompt_for_counter(counter: str, yes: bool) -> None:
    text = COUNTER_PROMPTS.get(counter, "Trigger this counter once on-device.")
    print(f"\n[{counter}] {text}")
    if yes:
        return
    input("Press Enter when ready; script will poll for increment...")


def main() -> int:
    args = parse_args()
    base_url = normalize_base_url(args.base_url)
    counters = split_counters(args)
    if not counters:
        print("No counters selected.")
        return 2

    print(f"Target URL: {base_url}")
    print(f"Counters ({len(counters)}): {', '.join(counters)}")

    destructive = [c for c in counters if c in DESTRUCTIVE_COUNTERS]
    if destructive:
        print(f"Warning: destructive counters selected: {', '.join(destructive)}")
        print("These may power off the device and interrupt Wi-Fi/API access.")

    try:
        baseline_metrics = fetch_metrics(base_url, args.http_timeout_s)
    except Exception as exc:  # noqa: BLE001 - CLI entry point
        print(f"Failed to fetch baseline metrics: {exc}")
        return 2

    baseline: Dict[str, int] = {}
    missing: List[str] = []
    for counter in counters:
        value = metric_as_int(baseline_metrics, counter)
        if value is None:
            missing.append(counter)
        else:
            baseline[counter] = value
    if missing:
        print(f"Missing/non-integer counters in /api/debug/metrics: {', '.join(missing)}")
        return 2

    print("Baseline captured.")
    results: List[CounterResult] = []
    required_csv_values: Dict[str, int] = {}

    for counter in counters:
        prompt_for_counter(counter, args.yes)
        result = wait_for_increment(
            base_url=base_url,
            counter=counter,
            start_value=baseline[counter],
            timeout_s=args.timeout_s,
            poll_interval_s=args.poll_interval_s,
            http_timeout_s=args.http_timeout_s,
            verbose=args.verbose,
        )
        results.append(result)
        if result.ok:
            baseline[counter] = result.end_value
            required_csv_values[counter] = result.end_value
            print(f"  PASS {counter}: {result.message} in {result.elapsed_s:.1f}s")
        else:
            print(f"  FAIL {counter}: {result.message} (last={result.end_value})")

    ok_counters = [r for r in results if r.ok]
    failed_counters = [r for r in results if not r.ok]

    csv_ok = True
    csv_msg = "skipped"
    if not args.no_csv_check and ok_counters:
        try:
            csv_name = choose_perf_file(base_url, args.http_timeout_s)
            print(f"\nCSV check file: {csv_name}")
            csv_ok, csv_msg = wait_for_csv_reflection(
                base_url=base_url,
                csv_name=csv_name,
                required_values=required_csv_values,
                timeout_s=args.csv_timeout_s,
                poll_interval_s=args.poll_interval_s,
                http_timeout_s=args.http_timeout_s,
                verbose=args.verbose,
            )
        except Exception as exc:  # noqa: BLE001 - CLI entry point
            csv_ok = False
            csv_msg = f"failed to verify CSV: {exc}"
    elif args.no_csv_check:
        csv_msg = "CSV verification disabled (--no-csv-check)"
    elif not ok_counters:
        csv_msg = "no successful counters to verify in CSV"

    print("\nSummary:")
    print(f"  Counter checks passed: {len(ok_counters)}/{len(results)}")
    print(f"  CSV check: {'PASS' if csv_ok else 'FAIL'} - {csv_msg}")

    if failed_counters:
        print("  Failed counters:")
        for item in failed_counters:
            print(f"    - {item.name}: {item.message}")

    return 0 if (not failed_counters and csv_ok) else 1


if __name__ == "__main__":
    raise SystemExit(main())
