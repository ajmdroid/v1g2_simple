#!/usr/bin/env python3
"""Capture web UI + API latency baseline from a running device.

This script is intentionally curl-based so it works in minimal dev environments
without extra Python dependencies.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import math
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTRACT = ROOT / "test" / "contracts" / "wifi_route_contract.txt"
DEFAULT_OUTPUT_DIR = ROOT / ".artifacts" / "web_baseline"

DEFAULT_UI_ROUTES = [
    "/",
    "/audio",
    "/autopush",
    "/colors",
    "/dev",
    "/devices",
    "/gps",
    "/integrations",
    "/lockouts",
    "/profiles",
    "/settings",
]

SKIP_GET_ROUTES = {
    "/api/debug/perf-files/download",  # requires a filename query parameter
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture web UI/API latency baselines from the device web server."
    )
    parser.add_argument(
        "--base-url",
        default="http://192.168.160.212",
        help="Base URL for device web server (default: %(default)s)",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=7,
        help="Samples per route (default: %(default)s)",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=2.0,
        help="curl connect timeout in seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--max-time",
        type=float,
        default=8.0,
        help="curl max request time in seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--sample-delay",
        type=float,
        default=0.05,
        help="Delay between samples in seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="Output directory for CSV/Markdown reports",
    )
    parser.add_argument(
        "--api-contract",
        default=str(DEFAULT_CONTRACT),
        help="Path to wifi_route_contract.txt used to discover GET routes",
    )
    parser.add_argument(
        "--no-api",
        action="store_true",
        help="Skip API route measurements",
    )
    parser.add_argument(
        "--no-ui",
        action="store_true",
        help="Skip UI route measurements",
    )
    return parser.parse_args()


def parse_get_routes(contract_path: Path) -> List[str]:
    if not contract_path.exists():
        return []

    routes: List[str] = []
    seen = set()
    with contract_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if not line.startswith("HTTP_GET "):
                continue
            parts = line.split()
            if len(parts) != 2:
                continue
            route = parts[1]
            if route in SKIP_GET_ROUTES:
                continue
            if route not in seen:
                seen.add(route)
                routes.append(route)
    return routes


def to_int(raw: str, default: int = 0) -> int:
    try:
        return int(float(raw))
    except (TypeError, ValueError):
        return default


def to_float(raw: str, default: float = 0.0) -> float:
    try:
        return float(raw)
    except (TypeError, ValueError):
        return default


def run_probe(url: str, connect_timeout_s: float, max_time_s: float) -> Dict[str, object]:
    fmt = (
        "http_code=%{http_code};"
        "size_download=%{size_download};"
        "time_connect=%{time_connect};"
        "time_starttransfer=%{time_starttransfer};"
        "time_total=%{time_total}"
    )
    cmd = [
        "curl",
        "--silent",
        "--show-error",
        "--location",
        "--output",
        "/dev/null",
        "--connect-timeout",
        str(connect_timeout_s),
        "--max-time",
        str(max_time_s),
        "--write-out",
        fmt,
        url,
    ]

    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )

    parsed: Dict[str, str] = {}
    for piece in proc.stdout.strip().split(";"):
        if "=" not in piece:
            continue
        key, value = piece.split("=", 1)
        parsed[key.strip()] = value.strip()

    return {
        "curl_exit_code": proc.returncode,
        "curl_error": proc.stderr.strip(),
        "http_code": to_int(parsed.get("http_code", "0")),
        "size_bytes": to_int(parsed.get("size_download", "0")),
        "connect_ms": to_float(parsed.get("time_connect", "0.0")) * 1000.0,
        "ttfb_ms": to_float(parsed.get("time_starttransfer", "0.0")) * 1000.0,
        "total_ms": to_float(parsed.get("time_total", "0.0")) * 1000.0,
    }


def percentile(values: Iterable[float], p: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    rank = max(1, math.ceil((p / 100.0) * len(ordered)))
    index = min(rank - 1, len(ordered) - 1)
    return ordered[index]


def format_ms(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.1f}"


def format_bytes(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.0f}"


def summarize_rows(rows: List[Dict[str, object]]) -> List[Dict[str, object]]:
    grouped: Dict[Tuple[str, str], List[Dict[str, object]]] = defaultdict(list)
    for row in rows:
        grouped[(str(row["kind"]), str(row["route"]))].append(row)

    summaries: List[Dict[str, object]] = []
    for (kind, route), items in sorted(grouped.items()):
        sampled = len(items)
        completed = [r for r in items if int(r["curl_exit_code"]) == 0]
        ok = [r for r in completed if 200 <= int(r["http_code"]) < 400]

        total_ms = [float(r["total_ms"]) for r in completed]
        ttfb_ms = [float(r["ttfb_ms"]) for r in completed]
        payloads = [float(r["size_bytes"]) for r in completed]
        code_hist = Counter(int(r["http_code"]) for r in completed)

        summaries.append(
            {
                "kind": kind,
                "route": route,
                "samples": sampled,
                "completed": len(completed),
                "ok": len(ok),
                "success_rate": (len(ok) / sampled * 100.0) if sampled else 0.0,
                "p50_total_ms": percentile(total_ms, 50),
                "p95_total_ms": percentile(total_ms, 95),
                "p50_ttfb_ms": percentile(ttfb_ms, 50),
                "p95_ttfb_ms": percentile(ttfb_ms, 95),
                "avg_bytes": (sum(payloads) / len(payloads)) if payloads else None,
                "codes": ", ".join(
                    f"{code}:{count}" for code, count in sorted(code_hist.items())
                )
                if code_hist
                else "none",
            }
        )

    return summaries


def write_csv(rows: List[Dict[str, object]], path: Path) -> None:
    fieldnames = [
        "timestamp_utc",
        "kind",
        "route",
        "url",
        "sample_index",
        "http_code",
        "size_bytes",
        "connect_ms",
        "ttfb_ms",
        "total_ms",
        "curl_exit_code",
        "curl_error",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_markdown(
    path: Path,
    *,
    generated_utc: str,
    base_url: str,
    samples: int,
    connect_timeout_s: float,
    max_time_s: float,
    preflight: Dict[str, object],
    summaries: List[Dict[str, object]],
) -> None:
    ui = [s for s in summaries if s["kind"] == "ui"]
    api = [s for s in summaries if s["kind"] == "api"]

    def render_table(rows: List[Dict[str, object]]) -> str:
        if not rows:
            return "No routes sampled."
        lines = [
            "| Route | Success | p50 total (ms) | p95 total (ms) | p50 TTFB (ms) | p95 TTFB (ms) | Avg bytes | Codes |",
            "|---|---:|---:|---:|---:|---:|---:|---|",
        ]
        for row in rows:
            lines.append(
                "| {route} | {ok}/{samples} ({success:.0f}%) | {p50_total} | {p95_total} | {p50_ttfb} | {p95_ttfb} | {avg_bytes} | {codes} |".format(
                    route=row["route"],
                    ok=row["ok"],
                    samples=row["samples"],
                    success=row["success_rate"],
                    p50_total=format_ms(row["p50_total_ms"]),
                    p95_total=format_ms(row["p95_total_ms"]),
                    p50_ttfb=format_ms(row["p50_ttfb_ms"]),
                    p95_ttfb=format_ms(row["p95_ttfb_ms"]),
                    avg_bytes=format_bytes(row["avg_bytes"]),
                    codes=row["codes"],
                )
            )
        return "\n".join(lines)

    preflight_line = (
        f"code={preflight['http_code']} total={format_ms(float(preflight['total_ms']))}ms "
        f"ttfb={format_ms(float(preflight['ttfb_ms']))}ms curl_exit={preflight['curl_exit_code']}"
    )

    content = "\n".join(
        [
            "# Web Baseline Snapshot",
            "",
            f"- Generated (UTC): {generated_utc}",
            f"- Base URL: `{base_url}`",
            f"- Samples per route: `{samples}`",
            f"- Timeouts: connect `{connect_timeout_s}s`, max `{max_time_s}s`",
            f"- Preflight (`/`): {preflight_line}",
            "",
            "## UI Routes",
            render_table(ui),
            "",
            "## API Routes (GET)",
            render_table(api),
            "",
            "## Notes",
            "- `p50`/`p95` are based on completed curl requests (curl exit code 0).",
            "- Success is HTTP 2xx/3xx over total sampled requests.",
            "- Use this file as the baseline before web UI overhaul; compare with a post-change run.",
        ]
    )

    path.write_text(content + "\n", encoding="utf-8")


def normalize_base_url(base_url: str) -> str:
    return base_url.rstrip("/")


def main() -> int:
    args = parse_args()

    if args.samples <= 0:
        print("[baseline] --samples must be > 0", file=sys.stderr)
        return 2

    if args.no_ui and args.no_api:
        print("[baseline] Nothing to run: both --no-ui and --no-api are set", file=sys.stderr)
        return 2

    base_url = normalize_base_url(args.base_url)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    api_routes: List[str] = []
    contract_path = Path(args.api_contract)
    if not args.no_api:
        api_routes = parse_get_routes(contract_path)
        if not api_routes:
            print(
                f"[baseline] warning: no GET routes discovered from {contract_path}; API sampling skipped",
                file=sys.stderr,
            )

    ui_routes = [] if args.no_ui else list(DEFAULT_UI_ROUTES)

    preflight = run_probe(
        f"{base_url}/",
        connect_timeout_s=args.connect_timeout,
        max_time_s=args.max_time,
    )
    if int(preflight["curl_exit_code"]) != 0:
        print(
            "[baseline] preflight failed for /: "
            f"curl_exit={preflight['curl_exit_code']} error={preflight['curl_error']}",
            file=sys.stderr,
        )
        return 1

    print(
        "[baseline] preflight ok: "
        f"code={preflight['http_code']} total={format_ms(float(preflight['total_ms']))}ms"
    )

    targets: List[Tuple[str, str]] = []
    targets.extend(("ui", route) for route in ui_routes)
    targets.extend(("api", route) for route in api_routes)

    rows: List[Dict[str, object]] = []
    now_utc = dt.datetime.now(dt.timezone.utc)

    for kind, route in targets:
        url = f"{base_url}{route}"
        print(f"[baseline] sampling {kind} {route} ({args.samples}x)")
        for sample_index in range(1, args.samples + 1):
            result = run_probe(
                url,
                connect_timeout_s=args.connect_timeout,
                max_time_s=args.max_time,
            )
            rows.append(
                {
                    "timestamp_utc": now_utc.isoformat(),
                    "kind": kind,
                    "route": route,
                    "url": url,
                    "sample_index": sample_index,
                    "http_code": result["http_code"],
                    "size_bytes": result["size_bytes"],
                    "connect_ms": f"{float(result['connect_ms']):.3f}",
                    "ttfb_ms": f"{float(result['ttfb_ms']):.3f}",
                    "total_ms": f"{float(result['total_ms']):.3f}",
                    "curl_exit_code": result["curl_exit_code"],
                    "curl_error": result["curl_error"],
                }
            )
            if sample_index < args.samples and args.sample_delay > 0:
                time.sleep(args.sample_delay)

    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d_%H%M%S")
    csv_path = output_dir / f"web_baseline_{timestamp}.csv"
    md_path = output_dir / f"web_baseline_{timestamp}.md"

    write_csv(rows, csv_path)
    summaries = summarize_rows(rows)
    write_markdown(
        md_path,
        generated_utc=dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ"),
        base_url=base_url,
        samples=args.samples,
        connect_timeout_s=args.connect_timeout,
        max_time_s=args.max_time,
        preflight=preflight,
        summaries=summaries,
    )

    print(f"[baseline] wrote CSV: {csv_path}")
    print(f"[baseline] wrote Markdown summary: {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
