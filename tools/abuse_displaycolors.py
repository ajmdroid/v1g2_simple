#!/usr/bin/env python3
"""
Abuse test for /api/displaycolors save path.

This repeatedly posts display settings saves at a controlled rate and
captures regression signals from /api/debug/metrics.
"""

from __future__ import annotations

import argparse
import json
import random
import statistics
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple


COLOR_FIELDS: List[str] = [
    "bogey",
    "freq",
    "arrowFront",
    "arrowSide",
    "arrowRear",
    "bandL",
    "bandKa",
    "bandK",
    "bandX",
    "bandPhoto",
    "wifiIcon",
    "wifiConnected",
    "bleConnected",
    "bleDisconnected",
    "bar1",
    "bar2",
    "bar3",
    "bar4",
    "bar5",
    "bar6",
    "muted",
    "persisted",
    "volumeMain",
    "volumeMute",
    "rssiV1",
    "rssiProxy",
]

BOOL_FIELDS: List[str] = [
    "freqUseBandColor",
    "hideWifiIcon",
    "hideProfileIndicator",
    "hideBatteryIcon",
    "showBatteryPercent",
    "hideBleIcon",
    "hideVolumeIndicator",
    "hideRssiIndicator",
    "showRestTelemetryCards",
]

DEFAULT_PALETTE: List[int] = [
    0xF800,  # red
    0x001F,  # blue
    0x07E0,  # green
    0xFFE0,  # yellow
    0x07FF,  # cyan
    0xFFFF,  # white
    0x0000,  # black
    0x780F,  # purple
    0x18C3,  # dark gray
    0x3186,  # gray
]

METRIC_FIELDS: List[str] = [
    "loopMaxUs",
    "wifiMaxUs",
    "fsMaxUs",
    "sdMaxUs",
    "flushMaxUs",
    "bleDrainMaxUs",
]

COUNTER_FIELDS: List[str] = [
    "queueDrops",
    "parseFailures",
    "disconnects",
    "reconnects",
    "perfDrop",
    "perfSdLockFail",
    "perfSdOpenFail",
    "perfSdWriteFail",
    "alertPersistStarts",
    "alertPersistExpires",
    "alertPersistClears",
    "autoPushStarts",
    "autoPushCompletes",
    "autoPushNoProfile",
    "autoPushProfileLoadFail",
    "autoPushProfileWriteFail",
    "autoPushBusyRetries",
    "autoPushModeFail",
    "autoPushVolumeFail",
    "autoPushDisconnectAbort",
    "speedVolBoosts",
    "speedVolRestores",
    "speedVolFadeTakeovers",
    "speedVolNoHeadroom",
    "voiceAnnouncePriority",
    "voiceAnnounceDirection",
    "voiceAnnounceSecondary",
    "voiceAnnounceEscalation",
    "voiceDirectionThrottled",
]


@dataclass
class PollRecord:
    elapsed_s: float
    metrics: Dict[str, int]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Abuse test /api/displaycolors save path")
    parser.add_argument("--base-url", default="http://192.168.35.1", help="Device base URL")
    parser.add_argument(
        "--iterations",
        type=int,
        default=90,
        help="Number of save requests to send (default 90)",
    )
    parser.add_argument(
        "--interval-ms",
        type=int,
        default=700,
        help="Delay between requests in ms (default 700)",
    )
    parser.add_argument(
        "--timeout-s",
        type=float,
        default=5.0,
        help="Per-request HTTP timeout in seconds",
    )
    parser.add_argument(
        "--poll-metrics-every-s",
        type=float,
        default=2.0,
        help="How often to poll /api/debug/metrics while running",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for deterministic payload variation",
    )
    parser.add_argument(
        "--with-preview",
        action="store_true",
        help="Do not set skipPreview=true on save requests",
    )
    parser.add_argument(
        "--json-out",
        default="",
        help="Optional path to write JSON summary",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print per-request status lines",
    )
    return parser.parse_args()


def normalize_base_url(url: str) -> str:
    return url.rstrip("/")


def http_get_json(base_url: str, path: str, timeout_s: float) -> Dict[str, Any]:
    url = f"{base_url}{path}"
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout_s) as resp:
        body = resp.read().decode("utf-8", errors="replace")
        return json.loads(body)


def http_post_form(
    base_url: str,
    path: str,
    payload: Dict[str, Any],
    timeout_s: float,
) -> Tuple[int, str]:
    url = f"{base_url}{path}"
    data = urllib.parse.urlencode(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return int(resp.status), body
    except urllib.error.HTTPError as err:
        body = err.read().decode("utf-8", errors="replace")
        return int(err.code), body


def get_metric_snapshot(base_url: str, timeout_s: float) -> Dict[str, int]:
    raw = http_get_json(base_url, "/api/debug/metrics", timeout_s)
    snap: Dict[str, int] = {}
    for key in METRIC_FIELDS + COUNTER_FIELDS:
        val = raw.get(key, 0)
        try:
            snap[key] = int(val)
        except (TypeError, ValueError):
            snap[key] = 0
    return snap


def safe_int(value: Any, fallback: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def load_base_colors(base_url: str, timeout_s: float) -> Dict[str, int]:
    try:
        raw = http_get_json(base_url, "/api/displaycolors", timeout_s)
    except Exception:
        return {}

    out: Dict[str, int] = {}
    for key in COLOR_FIELDS + BOOL_FIELDS + ["brightness"]:
        if key not in raw:
            continue
        if key in BOOL_FIELDS:
            out[key] = 1 if bool(raw[key]) else 0
        else:
            out[key] = safe_int(raw[key], 0)
    return out


def choose_color(i: int, idx: int, rng: random.Random) -> int:
    # Deterministic pattern plus tiny random perturbation.
    base = DEFAULT_PALETTE[(i + idx) % len(DEFAULT_PALETTE)]
    if rng.random() < 0.12:
        return DEFAULT_PALETTE[rng.randrange(len(DEFAULT_PALETTE))]
    return base


def bool_for_iteration(i: int, idx: int) -> bool:
    return ((i + idx) % 3) == 0


def build_payload(
    i: int,
    base_colors: Dict[str, int],
    rng: random.Random,
    skip_preview: bool,
) -> Dict[str, Any]:
    payload: Dict[str, Any] = {}

    for idx, field in enumerate(COLOR_FIELDS):
        payload[field] = choose_color(i, idx, rng)

    for idx, field in enumerate(BOOL_FIELDS):
        payload[field] = "true" if bool_for_iteration(i, idx) else "false"

    # Keep brightness in a moderate range to avoid extreme visual flicker.
    base_brightness = base_colors.get("brightness", 180)
    brightness = 120 + ((base_brightness + i * 7) % 90)
    payload["brightness"] = max(0, min(255, brightness))

    if skip_preview:
        payload["skipPreview"] = "true"
    return payload


def pct(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    rank = (len(values) - 1) * p
    lo = int(rank)
    hi = min(lo + 1, len(values) - 1)
    w = rank - lo
    return values[lo] * (1.0 - w) + values[hi] * w


def summarize_latencies(latencies_ms: List[float]) -> Dict[str, float]:
    if not latencies_ms:
        return {
            "min_ms": 0.0,
            "avg_ms": 0.0,
            "p50_ms": 0.0,
            "p95_ms": 0.0,
            "p99_ms": 0.0,
            "max_ms": 0.0,
        }
    ordered = sorted(latencies_ms)
    return {
        "min_ms": ordered[0],
        "avg_ms": statistics.fmean(ordered),
        "p50_ms": pct(ordered, 0.50),
        "p95_ms": pct(ordered, 0.95),
        "p99_ms": pct(ordered, 0.99),
        "max_ms": ordered[-1],
    }


def print_metric_line(label: str, value: int, threshold: int) -> bool:
    ok = value < threshold
    verdict = "OK" if ok else "FAIL"
    print(f"{label:14s} {value:8d} us  (threshold < {threshold}) [{verdict}]")
    return ok


def main() -> int:
    args = parse_args()
    base_url = normalize_base_url(args.base_url)
    rng = random.Random(args.seed)
    skip_preview = not args.with_preview

    if args.iterations <= 0:
        print("iterations must be > 0")
        return 2
    if args.interval_ms < 0:
        print("interval-ms must be >= 0")
        return 2

    print(f"Target: {base_url}")
    print(
        f"Run: iterations={args.iterations} interval_ms={args.interval_ms} "
        f"skip_preview={skip_preview} seed={args.seed}"
    )

    base_colors = load_base_colors(base_url, args.timeout_s)

    start_metrics: Optional[Dict[str, int]] = None
    try:
        start_metrics = get_metric_snapshot(base_url, args.timeout_s)
        print("Metrics baseline: captured")
    except Exception as err:
        print(f"Metrics baseline: unavailable ({err})")

    latencies_ms: List[float] = []
    status_counts: Dict[int, int] = {}
    failures: List[str] = []
    polls: List[PollRecord] = []

    t0 = time.monotonic()
    next_poll = t0

    for i in range(args.iterations):
        payload = build_payload(i, base_colors, rng, skip_preview)

        req_start = time.monotonic()
        status = 0
        body = ""
        try:
            status, body = http_post_form(base_url, "/api/displaycolors", payload, args.timeout_s)
        except Exception as err:
            failures.append(f"req#{i + 1}: exception={err}")

        elapsed_ms = (time.monotonic() - req_start) * 1000.0
        latencies_ms.append(elapsed_ms)
        status_counts[status] = status_counts.get(status, 0) + 1

        if status != 200 and len(failures) < 20:
            snippet = body.strip().replace("\n", " ")
            failures.append(f"req#{i + 1}: status={status} body={snippet[:120]}")

        if args.verbose:
            print(f"req={i + 1:04d} status={status:3d} latency_ms={elapsed_ms:8.2f}")

        now = time.monotonic()
        if now >= next_poll:
            try:
                snap = get_metric_snapshot(base_url, args.timeout_s)
                polls.append(PollRecord(elapsed_s=now - t0, metrics=snap))
            except Exception:
                pass
            next_poll = now + max(0.2, args.poll_metrics_every_s)

        if i < args.iterations - 1 and args.interval_ms > 0:
            time.sleep(args.interval_ms / 1000.0)

    runtime_s = time.monotonic() - t0

    end_metrics: Optional[Dict[str, int]] = None
    try:
        end_metrics = get_metric_snapshot(base_url, args.timeout_s)
    except Exception as err:
        failures.append(f"end-metrics exception={err}")

    latency_summary = summarize_latencies(latencies_ms)

    observed_metric_max: Dict[str, int] = {k: 0 for k in METRIC_FIELDS}
    for poll in polls:
        for key in METRIC_FIELDS:
            observed_metric_max[key] = max(observed_metric_max[key], poll.metrics.get(key, 0))
    if end_metrics:
        for key in METRIC_FIELDS:
            observed_metric_max[key] = max(observed_metric_max[key], end_metrics.get(key, 0))

    counter_delta: Dict[str, int] = {}
    if start_metrics and end_metrics:
        for key in COUNTER_FIELDS:
            counter_delta[key] = end_metrics.get(key, 0) - start_metrics.get(key, 0)

    total_reqs = len(latencies_ms)
    ok_reqs = status_counts.get(200, 0)
    fail_reqs = total_reqs - ok_reqs
    req_rate = (total_reqs / runtime_s) if runtime_s > 0 else 0.0

    print()
    print("=== Abuse Test Summary ===")
    print(f"requests_total:    {total_reqs}")
    print(f"requests_ok:       {ok_reqs}")
    print(f"requests_failed:   {fail_reqs}")
    print(f"http_429:          {status_counts.get(429, 0)}")
    print(f"runtime_s:         {runtime_s:.2f}")
    print(f"effective_req_s:   {req_rate:.2f}")
    print(
        "latency_ms:        "
        f"min={latency_summary['min_ms']:.1f} "
        f"avg={latency_summary['avg_ms']:.1f} "
        f"p50={latency_summary['p50_ms']:.1f} "
        f"p95={latency_summary['p95_ms']:.1f} "
        f"p99={latency_summary['p99_ms']:.1f} "
        f"max={latency_summary['max_ms']:.1f}"
    )

    if counter_delta:
        print()
        print("counter_deltas:")
        for key in COUNTER_FIELDS:
            print(f"  {key:16s} {counter_delta.get(key, 0)}")
    else:
        print()
        print("counter_deltas: unavailable")

    print()
    print("peak_metrics:")
    loop_ok = print_metric_line("loopMaxUs", observed_metric_max["loopMaxUs"], 500_000)
    wifi_ok = print_metric_line("wifiMaxUs", observed_metric_max["wifiMaxUs"], 150_000)
    fs_ok = print_metric_line("fsMaxUs", observed_metric_max["fsMaxUs"], 50_000)
    sd_ok = print_metric_line("sdMaxUs", observed_metric_max["sdMaxUs"], 50_000)
    print(f"{'flushMaxUs':14s} {observed_metric_max['flushMaxUs']:8d} us")
    print(f"{'bleDrainMaxUs':14s} {observed_metric_max['bleDrainMaxUs']:8d} us")

    no_qdrop = True
    no_parse_fail = True
    if counter_delta:
        no_qdrop = counter_delta.get("queueDrops", 0) == 0
        no_parse_fail = counter_delta.get("parseFailures", 0) == 0

    pass_all = (
        fail_reqs == 0
        and no_qdrop
        and no_parse_fail
        and loop_ok
        and wifi_ok
        and fs_ok
        and sd_ok
    )

    print()
    print(f"verdict: {'PASS' if pass_all else 'FAIL'}")
    if not no_qdrop:
        print("reason: queueDrops increased")
    if not no_parse_fail:
        print("reason: parseFailures increased")
    if fail_reqs:
        print("reason: HTTP request failures occurred")
    if not loop_ok:
        print("reason: loopMaxUs exceeded threshold")
    if not wifi_ok:
        print("reason: wifiMaxUs exceeded threshold")
    if not fs_ok:
        print("reason: fsMaxUs exceeded threshold")
    if not sd_ok:
        print("reason: sdMaxUs exceeded threshold")

    if failures:
        print()
        print("notable_failures:")
        for msg in failures[:20]:
            print(f"  {msg}")

    summary = {
        "base_url": base_url,
        "iterations": args.iterations,
        "interval_ms": args.interval_ms,
        "skip_preview": skip_preview,
        "runtime_s": runtime_s,
        "requests_total": total_reqs,
        "requests_ok": ok_reqs,
        "requests_failed": fail_reqs,
        "status_counts": status_counts,
        "latency_ms": latency_summary,
        "counter_delta": counter_delta,
        "peak_metrics": observed_metric_max,
        "verdict": "PASS" if pass_all else "FAIL",
        "failures": failures[:100],
    }

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
        print(f"\nWrote summary: {args.json_out}")

    return 0 if pass_all else 1


if __name__ == "__main__":
    raise SystemExit(main())
