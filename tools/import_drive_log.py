#!/usr/bin/env python3
"""Import a captured drive metrics log into the canonical hardware scoring artifacts."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import score_hardware_run  # type: ignore
from hardware_report_utils import write_comparison_text, write_comparison_tsv  # type: ignore


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"
SOAK_PARSE_METRICS = ROOT / "tools" / "soak_parse_metrics.py"

METRIC_UNITS = {
    "metrics_ok_samples": "count",
    "rx_packets_delta": "count",
    "parse_successes_delta": "count",
    "parse_failures_delta": "count",
    "queue_drops_delta": "count",
    "perf_drop_delta": "count",
    "event_drop_delta": "count",
    "oversize_drops_delta": "count",
    "display_updates_delta": "count",
    "display_skips_delta": "count",
    "reconnects_delta": "count",
    "disconnects_delta": "count",
    "gps_obs_drops_delta": "count",
    "flush_max_peak_us": "us",
    "loop_max_peak_us": "us",
    "wifi_max_peak_us": "us",
    "ble_drain_max_peak_us": "us",
    "sd_max_peak_us": "us",
    "fs_max_peak_us": "us",
    "queue_high_water_peak": "count",
    "wifi_connect_deferred_delta": "count",
    "dma_free_min_bytes": "bytes",
    "dma_largest_min_bytes": "bytes",
    "ble_process_max_peak_us": "us",
    "disp_pipe_max_peak_us": "us",
    "ble_mutex_timeout_delta": "count",
    "wifi_p95_us": "us",
    "disp_pipe_p95_us": "us",
    "dma_fragmentation_pct_p95": "percent",
    "samples_to_stable": "count",
    "time_to_stable_ms": "ms",
}

KV_ALIASES = {
    "metrics_ok_samples": "ok_samples",
    "rx_packets_delta": "rx_packets_delta",
    "parse_successes_delta": "parse_successes_delta",
    "parse_failures_delta": "parse_failures_delta",
    "queue_drops_delta": "queue_drops_delta",
    "perf_drop_delta": "perf_drop_delta",
    "event_drop_delta": "event_drop_delta",
    "oversize_drops_delta": "oversize_drops_delta",
    "display_updates_delta": "display_updates_delta",
    "display_skips_delta": "display_skips_delta",
    "reconnects_delta": "reconnects_delta",
    "disconnects_delta": "disconnects_delta",
    "gps_obs_drops_delta": "gps_obs_drops_delta",
    "flush_max_peak_us": "flush_max_peak",
    "loop_max_peak_us": "loop_max_peak",
    "ble_drain_max_peak_us": "ble_drain_max_peak",
    "sd_max_peak_us": "sd_max_peak",
    "fs_max_peak_us": "fs_max_peak",
    "queue_high_water_peak": "queue_high_water_peak",
    "wifi_connect_deferred_delta": "wifi_connect_deferred_delta",
    "dma_free_min_bytes": "dma_free_min",
    "dma_largest_min_bytes": "dma_largest_min",
    "ble_process_max_peak_us": "ble_process_max_peak",
    "disp_pipe_max_peak_us": "disp_pipe_max_peak",
    "ble_mutex_timeout_delta": "ble_mutex_timeout_delta",
    "disp_pipe_p95_us": "disp_pipe_p95",
    "dma_fragmentation_pct_p95": "dma_fragmentation_pct_p95",
    "samples_to_stable": "samples_to_stable",
    "time_to_stable_ms": "time_to_stable_ms",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Path to metrics.jsonl or a soak run directory containing it")
    parser.add_argument("--out-dir", required=True, help="Output directory for manifest/scoring artifacts")
    parser.add_argument("--compare-to", default="", help="Optional baseline manifest.json")
    parser.add_argument("--board-id", default="", help="Override board_id in emitted manifest")
    parser.add_argument("--git-sha", default="", help="Override git_sha in emitted manifest")
    parser.add_argument("--git-ref", default="", help="Override git_ref in emitted manifest")
    parser.add_argument("--suite-or-profile", default="", help="Override suite_or_profile in emitted manifest")
    parser.add_argument("--stress-class", default="", help="Override stress_class in emitted manifest")
    parser.add_argument("--env", default="", help="Override env in emitted manifest")
    parser.add_argument("--lane", default="real-fw-soak-import", help="Lane value for emitted manifest")
    return parser.parse_args()


def resolve_source(path: Path) -> tuple[Path, Path | None]:
    if path.is_file():
        manifest = path.parent / "manifest.json"
        return path, manifest if manifest.exists() else None
    if path.is_dir():
        direct_metrics = path / "metrics.jsonl"
        if direct_metrics.exists():
            manifest = path / "manifest.json"
            return direct_metrics, manifest if manifest.exists() else None
    raise RuntimeError(f"Could not resolve metrics.jsonl from '{path}'")


def load_json(path: Path | None) -> dict[str, Any] | None:
    if path is None or not path.exists():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"Expected JSON object in {path}")
    return payload


def parse_kv_output(raw_text: str) -> dict[str, str]:
    payload: dict[str, str] = {}
    for line in raw_text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        payload[key.strip()] = value.strip()
    return payload


def numeric(value: str) -> float | None:
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def select_wifi_peak_metric(kv: dict[str, str]) -> float | None:
    ok_samples = numeric(kv.get("ok_samples", ""))
    warm_value = numeric(kv.get("wifi_max_peak_excluding_first", ""))
    if ok_samples is not None and ok_samples > 2 and warm_value is not None:
        return warm_value
    return numeric(kv.get("wifi_max_peak", ""))


def select_wifi_p95_metric(kv: dict[str, str]) -> float | None:
    sample_count_excluding_first = numeric(kv.get("wifi_sample_count_excluding_first", ""))
    warm_value = numeric(kv.get("wifi_p95_excluding_first", ""))
    if sample_count_excluding_first is not None and sample_count_excluding_first > 0 and warm_value is not None:
        return warm_value
    return numeric(kv.get("wifi_p95_raw", ""))


def render_metrics_ndjson(out_path: Path, run_id: str, git_sha: str, suite_or_profile: str, kv: dict[str, str]) -> int:
    count = 0
    with out_path.open("w", encoding="utf-8") as handle:
        for key, unit in METRIC_UNITS.items():
            if key == "wifi_max_peak_us":
                value = select_wifi_peak_metric(kv)
            elif key == "wifi_p95_us":
                value = select_wifi_p95_metric(kv)
            else:
                source_key = KV_ALIASES.get(key, key)
                value = numeric(kv.get(source_key, ""))
            if value is None:
                continue
            record = {
                "schema_version": 1,
                "run_id": run_id,
                "git_sha": git_sha,
                "run_kind": "real_fw_soak",
                "suite_or_profile": suite_or_profile,
                "metric": key,
                "sample": "value",
                "value": value,
                "unit": unit,
                "tags": {},
            }
            handle.write(json.dumps(record, sort_keys=True))
            handle.write("\n")
            count += 1
    return count


def exit_code_for_result(result: str) -> int:
    if result == "FAIL":
        return 2
    if result == "PASS_WITH_WARNINGS":
        return 1
    return 0


def main() -> int:
    args = parse_args()
    source_input = Path(args.input).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    metrics_jsonl, source_manifest_path = resolve_source(source_input)
    source_manifest = load_json(source_manifest_path)

    parse_proc = subprocess.run(
        [sys.executable, str(SOAK_PARSE_METRICS), str(metrics_jsonl)],
        check=False,
        capture_output=True,
        text=True,
        cwd=ROOT,
    )
    if parse_proc.returncode != 0:
        print(parse_proc.stderr, file=sys.stderr, end="")
        return parse_proc.returncode

    kv = parse_kv_output(parse_proc.stdout)
    parsed_kv_path = out_dir / "parsed_metrics_kv.txt"
    parsed_kv_path.write_text(parse_proc.stdout, encoding="utf-8")

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    git_sha = args.git_sha or str((source_manifest or {}).get("git_sha") or "unknown")
    git_ref = args.git_ref or str((source_manifest or {}).get("git_ref") or "unknown")
    board_id = args.board_id or str((source_manifest or {}).get("board_id") or "unknown")
    suite_or_profile = args.suite_or_profile or str((source_manifest or {}).get("suite_or_profile") or "drive_wifi_ap")
    stress_class = args.stress_class or str((source_manifest or {}).get("stress_class") or "core")
    env_name = args.env or str((source_manifest or {}).get("env") or "parsed-drive-log")
    run_id = f"parsed_drive_log_{timestamp}_{git_sha}"

    metrics_ndjson = out_dir / "metrics.ndjson"
    metric_count = render_metrics_ndjson(metrics_ndjson, run_id, git_sha, suite_or_profile, kv)

    metrics_ok_samples = numeric(kv.get("ok_samples", ""))
    base_result = "PASS"
    if metric_count == 0 or metrics_ok_samples is None or metrics_ok_samples <= 0:
        base_result = "INCONCLUSIVE"

    manifest_path = out_dir / "manifest.json"
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "git_sha": git_sha,
        "git_ref": git_ref,
        "run_kind": "real_fw_soak",
        "board_id": board_id,
        "env": env_name,
        "lane": args.lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "result": base_result,
        "base_result": base_result,
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
        "tracks": [suite_or_profile] if metric_count > 0 else [],
        "source_input": str(source_input),
        "source_metrics_jsonl": str(metrics_jsonl),
    }
    if source_manifest_path is not None:
        manifest["source_manifest"] = str(source_manifest_path)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    baseline_path = Path(args.compare_to).resolve() if args.compare_to else None
    try:
        scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    scoring_path = out_dir / "scoring.json"
    comparison_txt = out_dir / "comparison.txt"
    comparison_tsv = out_dir / "comparison.tsv"
    scoring_path.write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    write_comparison_text(scored, comparison_txt)
    write_comparison_tsv(scored, comparison_tsv)

    manifest["result"] = scored["result"]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return exit_code_for_result(str(scored["result"]))


if __name__ == "__main__":
    raise SystemExit(main())
