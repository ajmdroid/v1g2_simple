#!/usr/bin/env python3
"""Import a perf CSV capture into the canonical hardware scoring pipeline."""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

import import_drive_log  # type: ignore
import score_hardware_run  # type: ignore
import score_perf_csv  # type: ignore
from hardware_report_utils import write_comparison_text, write_comparison_tsv  # type: ignore


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"
SLO_FILE = ROOT / "tools" / "perf_slo_thresholds.json"
DEFAULT_HEADER_COLUMNS = [
    line.strip()
    for line in (ROOT / "test" / "contracts" / "perf_csv_column_contract.txt").read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.startswith("#")
]
CURRENT_PERF_CSV_SCHEMA = 13
ALWAYS_UNSUPPORTED_METRICS = {"samples_to_stable", "time_to_stable_ms"}
LEGACY_UNSUPPORTED_METRICS = {"perf_drop_delta", "event_drop_delta"}
PEAK_COLUMNS = {
    "loop_max_peak_us": "loopMax_us",
    "ble_process_max_peak_us": "bleProcessMax_us",
    "wifi_max_peak_us": "wifiMax_us",
    "disp_pipe_max_peak_us": "dispPipeMax_us",
}
DELTA_COLUMNS = {
    "rx_packets_delta": "rx",
    "parse_successes_delta": "parseOK",
    "parse_failures_delta": "parseFail",
    "queue_drops_delta": "qDrop",
    "perf_drop_delta": "perfDrop",
    "event_drop_delta": "eventBusDrops",
    "oversize_drops_delta": "oversizeDrops",
    "display_updates_delta": "displayUpdates",
    "display_skips_delta": "displaySkips",
    "reconnects_delta": "reconn",
    "disconnects_delta": "disc",
    "gps_obs_drops_delta": "gpsObsDrops",
    "ble_mutex_timeout_delta": "bleMutexTimeout",
    "wifi_connect_deferred_delta": "wifiConnectDeferred",
}
PEAK_ONLY_COLUMNS = {
    "loop_max_peak_us": "loopMax_us",
    "flush_max_peak_us": "flushMax_us",
    "wifi_max_peak_us": "wifiMax_us",
    "ble_drain_max_peak_us": "bleDrainMax_us",
    "sd_max_peak_us": "sdMax_us",
    "fs_max_peak_us": "fsMax_us",
    "queue_high_water_peak": "queueHighWater",
    "ble_process_max_peak_us": "bleProcessMax_us",
    "disp_pipe_max_peak_us": "dispPipeMax_us",
}
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
    "ble_mutex_timeout_delta": "count",
    "wifi_connect_deferred_delta": "count",
    "loop_max_peak_us": "us",
    "flush_max_peak_us": "us",
    "wifi_max_peak_us": "us",
    "ble_drain_max_peak_us": "us",
    "sd_max_peak_us": "us",
    "fs_max_peak_us": "us",
    "queue_high_water_peak": "count",
    "ble_process_max_peak_us": "us",
    "disp_pipe_max_peak_us": "us",
    "dma_free_min_bytes": "bytes",
    "dma_largest_min_bytes": "bytes",
    "wifi_p95_us": "us",
    "disp_pipe_p95_us": "us",
    "dma_fragmentation_pct_p95": "pct",
}


@dataclass(frozen=True)
class SessionSummary:
    session_index: int
    token: str
    schema: int
    row_count: int
    duration_ms: int
    duration_s: float
    rx_delta: int
    speed_active_rows: int
    gps_valid_rows: int
    connected: bool
    drive_like: bool
    has_marker: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "session_index": self.session_index,
            "token": self.token,
            "schema": self.schema,
            "row_count": self.row_count,
            "duration_ms": self.duration_ms,
            "duration_s": self.duration_s,
            "rx_delta": self.rx_delta,
            "speed_active_rows": self.speed_active_rows,
            "gps_valid_rows": self.gps_valid_rows,
            "connected": self.connected,
            "drive_like": self.drive_like,
            "has_marker": self.has_marker,
        }


def _percentile(values: list[float], pct: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(ordered[lo])
    frac = rank - lo
    return float(ordered[lo] + (ordered[hi] - ordered[lo]) * frac)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="Path to perf capture CSV")
    parser.add_argument("--out-dir", default="", help="Output directory for manifest/scoring artifacts")
    parser.add_argument(
        "--profile",
        default="drive_wifi_off",
        help="SLO profile: drive_wifi_off (default) or drive_wifi_ap",
    )
    parser.add_argument(
        "--segment",
        default="",
        help="Drive segment selector: auto (default), last, longest-connected, or 1-based index",
    )
    parser.add_argument(
        "--session",
        default="",
        help="Compatibility alias for --segment",
    )
    parser.add_argument("--list-segments", action="store_true", help="List discovered CSV segments and exit")
    parser.add_argument("--compare-to", default="", help="Optional baseline manifest.json")
    parser.add_argument("--board-id", default="", help="Override board_id")
    parser.add_argument("--git-sha", default="", help="Override git_sha")
    parser.add_argument("--git-ref", default="", help="Override git_ref")
    parser.add_argument("--stress-class", default="core", help="Stress class (default: core)")
    parser.add_argument("--lane", default="perf-csv-import", help="Lane tag")
    args = parser.parse_args()
    if not args.list_segments and not args.out_dir:
        parser.error("--out-dir is required unless --list-segments is used")
    return args


def _selector_arg(args: argparse.Namespace) -> str:
    return (args.segment or args.session or "auto").strip() or "auto"


def load_sessions(path: Path) -> list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]]:
    sessions: list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]] = []
    current_fields: Optional[list[str]] = None
    current_meta: Optional[score_perf_csv.SessionMeta] = None
    current_rows: list[dict[str, int]] = []
    pending_rows: list[str] = []

    def parse_rows(raw_rows: list[str], fields: list[str]) -> list[dict[str, int]]:
        parsed_rows: list[dict[str, int]] = []
        for raw_line in raw_rows:
            if not raw_line or raw_line.startswith("#"):
                continue
            values = raw_line.split(",")
            millis_val = values[0].strip() if values else ""
            if not millis_val or millis_val.startswith("#"):
                continue
            row: dict[str, int] = {}
            for index, key in enumerate(fields):
                raw_value = values[index].strip() if index < len(values) else "0"
                row[key] = score_perf_csv.parse_int(raw_value or "0")
            parsed_rows.append(row)
        return parsed_rows

    with path.open("r", newline="", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("millis,"):
                fields = line.split(",")
                if current_fields is None and pending_rows:
                    leading_rows = parse_rows(pending_rows, fields)
                    if leading_rows:
                        sessions.append((None, leading_rows))
                    pending_rows = []
                elif current_fields is not None and current_rows:
                    sessions.append((current_meta, current_rows))
                current_fields = fields
                current_meta = None
                current_rows = []
                continue

            if line.startswith("#session_start"):
                if current_fields is None:
                    pending_rows.append(line)
                else:
                    current_meta = score_perf_csv._parse_session_meta(line)
                continue

            if current_fields is None:
                pending_rows.append(line)
                continue

            current_rows.extend(parse_rows([line], current_fields))

    if current_fields is None and pending_rows:
        header_fields = list(DEFAULT_HEADER_COLUMNS)
        trailing_rows = parse_rows(pending_rows, header_fields)
        if trailing_rows:
            sessions.append((None, trailing_rows))
    elif current_fields is not None and current_rows:
        sessions.append((current_meta, current_rows))

    return sessions


def _duration_ms(rows: list[dict[str, int]]) -> int:
    if not rows:
        return 0
    return max(0, int(rows[-1].get("millis", 0)) - int(rows[0].get("millis", 0)))


def _rx_delta(rows: list[dict[str, int]]) -> int:
    if not rows:
        return 0
    return int(rows[-1].get("rx", 0)) - int(rows[0].get("rx", 0))


def summarize_sessions(
    sessions: list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]]
) -> list[SessionSummary]:
    summaries: list[SessionSummary] = []
    for index, (meta, rows) in enumerate(sessions, start=1):
        rx_delta = _rx_delta(rows)
        speed_active_rows = sum(1 for row in rows if int(row.get("gpsSpeedMph_x10", 0)) > 0)
        gps_valid_rows = sum(
            1 for row in rows if int(row.get("gpsHasFix", 0)) == 1 and int(row.get("gpsLocationValid", 0)) == 1
        )
        connected = bool(rows) and (rx_delta > 0 or int(rows[-1].get("rx", 0)) > 0)
        summaries.append(
            SessionSummary(
                session_index=index,
                token=meta.token if meta and meta.token else "",
                schema=int(meta.schema) if meta else 0,
                row_count=len(rows),
                duration_ms=_duration_ms(rows),
                duration_s=score_perf_csv.duration_s(rows) if rows else 0.0,
                rx_delta=rx_delta,
                speed_active_rows=speed_active_rows,
                gps_valid_rows=gps_valid_rows,
                connected=connected,
                drive_like=(speed_active_rows > 0 or gps_valid_rows > 0),
                has_marker=meta is not None,
            )
        )
    return summaries


def select_segment(
    sessions: list[tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]]]],
    selector: str,
) -> tuple[Optional[score_perf_csv.SessionMeta], list[dict[str, int]], SessionSummary, list[SessionSummary], str]:
    summaries = summarize_sessions(sessions)
    if not summaries:
        raise RuntimeError("No sessions found in CSV")

    effective_selector = selector
    if selector == "auto":
        drive_candidates = [summary for summary in summaries if summary.connected and summary.drive_like]
        if drive_candidates:
            chosen = max(
                drive_candidates,
                key=lambda item: (
                    item.speed_active_rows,
                    item.gps_valid_rows,
                    item.rx_delta,
                    item.duration_ms,
                    item.session_index,
                ),
            )
        else:
            try:
                _meta, _rows, idx = score_perf_csv.select_session(sessions, "longest-connected")
            except RuntimeError:
                _meta, _rows, idx = score_perf_csv.select_session(sessions, "longest")
            chosen = summaries[idx - 1]
        effective_selector = f"auto->{chosen.session_index}"
        return sessions[chosen.session_index - 1][0], sessions[chosen.session_index - 1][1], chosen, summaries, effective_selector

    meta, rows, session_index = score_perf_csv.select_session(sessions, selector)
    return meta, rows, summaries[session_index - 1], summaries, selector


def render_segment_listing(
    summaries: list[SessionSummary], selected_index: int, selector: str, csv_path: Path
) -> str:
    rows = [
        [
            "*" if summary.session_index == selected_index else "",
            str(summary.session_index),
            summary.token or "n/a",
            str(summary.schema or 0),
            str(summary.row_count),
            f"{summary.duration_s:.1f}",
            str(summary.rx_delta),
            str(summary.speed_active_rows),
            str(summary.gps_valid_rows),
            "yes" if summary.drive_like else "no",
        ]
        for summary in summaries
    ]
    widths = [len(label) for label in ["SEL", "SEG", "TOKEN", "SCHEMA", "ROWS", "DUR_S", "RX_DELTA", "SPEED_ROWS", "GPS_ROWS", "DRIVE"]]
    for row in rows:
        for idx, value in enumerate(row):
            widths[idx] = max(widths[idx], len(value))

    def fmt(values: list[str]) -> str:
        return "  ".join(value.ljust(widths[idx]) for idx, value in enumerate(values))

    lines = [
        f"source: {csv_path}",
        f"selector: {selector}",
        "",
        fmt(["SEL", "SEG", "TOKEN", "SCHEMA", "ROWS", "DUR_S", "RX_DELTA", "SPEED_ROWS", "GPS_ROWS", "DRIVE"]),
        fmt(["-" * widths[0], "-" * widths[1], "-" * widths[2], "-" * widths[3], "-" * widths[4], "-" * widths[5], "-" * widths[6], "-" * widths[7], "-" * widths[8], "-" * widths[9]]),
    ]
    lines.extend(fmt(row) for row in rows)
    return "\n".join(lines) + "\n"


def _has_column(rows: list[dict[str, int]], column: str) -> bool:
    return bool(rows) and column in rows[0]


def _delta_metric(rows: list[dict[str, int]], column: str) -> Optional[float]:
    if not _has_column(rows, column):
        return None
    return float(int(rows[-1].get(column, 0)) - int(rows[0].get(column, 0)))


def _peak_metric(rows: list[dict[str, int]], column: str) -> Optional[float]:
    if not _has_column(rows, column):
        return None
    return float(max(int(row.get(column, 0)) for row in rows))


def _floor_metric(rows: list[dict[str, int]], column: str) -> Optional[float]:
    if not _has_column(rows, column):
        return None
    return float(min(int(row.get(column, 0)) for row in rows))


def _peak_diagnostic(rows: list[dict[str, int]], column: str) -> Optional[dict[str, Any]]:
    if not _has_column(rows, column):
        return None
    peak_index, peak_row = max(enumerate(rows), key=lambda item: int(item[1].get(column, 0)))
    return {
        "column": column,
        "value": float(int(peak_row.get(column, 0))),
        "row_index": peak_index + 1,
        "millis": int(peak_row.get("millis", 0)),
    }


def extract_metrics(
    rows: list[dict[str, int]],
    source_schema: int,
) -> tuple[dict[str, tuple[float, str]], dict[str, dict[str, Any]], list[str]]:
    if not rows:
        return {}, {}, []

    metrics: dict[str, tuple[float, str]] = {
        "metrics_ok_samples": (float(len(rows)), METRIC_UNITS["metrics_ok_samples"])
    }
    unsupported_metrics = set(ALWAYS_UNSUPPORTED_METRICS)
    columns = set(rows[0].keys())
    if source_schema < CURRENT_PERF_CSV_SCHEMA or not {"perfDrop", "eventBusDrops"} <= columns:
        unsupported_metrics.update(LEGACY_UNSUPPORTED_METRICS)

    for metric_name, column in DELTA_COLUMNS.items():
        if metric_name in unsupported_metrics:
            continue
        value = _delta_metric(rows, column)
        if value is not None:
            metrics[metric_name] = (value, METRIC_UNITS[metric_name])

    peak_diagnostics: dict[str, dict[str, Any]] = {}
    for metric_name, column in PEAK_ONLY_COLUMNS.items():
        value = _peak_metric(rows, column)
        if value is None:
            continue
        metrics[metric_name] = (value, METRIC_UNITS[metric_name])
        if metric_name in PEAK_COLUMNS:
            diagnostic = _peak_diagnostic(rows, column)
            if diagnostic is not None:
                peak_diagnostics[metric_name] = diagnostic

    free_dma_floor_column = "freeDmaMin" if _has_column(rows, "freeDmaMin") else "freeDma"
    largest_dma_floor_column = "largestDmaMin" if _has_column(rows, "largestDmaMin") else "largestDma"
    dma_free_floor = _floor_metric(rows, free_dma_floor_column)
    dma_largest_floor = _floor_metric(rows, largest_dma_floor_column)
    if dma_free_floor is not None:
        metrics["dma_free_min_bytes"] = (dma_free_floor, METRIC_UNITS["dma_free_min_bytes"])
    if dma_largest_floor is not None:
        metrics["dma_largest_min_bytes"] = (dma_largest_floor, METRIC_UNITS["dma_largest_min_bytes"])

    if _has_column(rows, "wifiMax_us"):
        wifi_samples = [float(int(row.get("wifiMax_us", 0))) for row in rows]
        wifi_p95 = _percentile(wifi_samples, 95)
        if wifi_p95 is not None:
            metrics["wifi_p95_us"] = (wifi_p95, METRIC_UNITS["wifi_p95_us"])

    if _has_column(rows, "dispPipeMax_us"):
        disp_samples = [float(int(row.get("dispPipeMax_us", 0))) for row in rows]
        disp_p95 = _percentile(disp_samples, 95)
        if disp_p95 is not None:
            metrics["disp_pipe_p95_us"] = (disp_p95, METRIC_UNITS["disp_pipe_p95_us"])

    if _has_column(rows, "freeDma") and _has_column(rows, "largestDma"):
        fragmentation_samples: list[float] = []
        for row in rows:
            free_dma = float(int(row.get("freeDma", 0)))
            largest_dma = float(int(row.get("largestDma", 0)))
            if free_dma > 0:
                fragmentation_samples.append((1.0 - (largest_dma / free_dma)) * 100.0)
        fragmentation_p95 = _percentile(fragmentation_samples, 95)
        if fragmentation_p95 is not None:
            metrics["dma_fragmentation_pct_p95"] = (
                fragmentation_p95,
                METRIC_UNITS["dma_fragmentation_pct_p95"],
            )

    return metrics, peak_diagnostics, sorted(unsupported_metrics)


def write_metrics_ndjson(
    path: Path,
    run_id: str,
    git_sha: str,
    suite_or_profile: str,
    metrics: dict[str, tuple[float, str]],
) -> int:
    count = 0
    with path.open("w", encoding="utf-8") as handle:
        for metric_name, (value, unit) in sorted(metrics.items()):
            handle.write(
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": run_id,
                        "git_sha": git_sha,
                        "run_kind": "real_fw_soak",
                        "suite_or_profile": suite_or_profile,
                        "metric": metric_name,
                        "sample": "value",
                        "value": value,
                        "unit": unit,
                        "tags": {},
                    },
                    sort_keys=True,
                )
            )
            handle.write("\n")
            count += 1
    return count


def run_csv_scorecard(
    profile: str,
    session_selector: str,
    rows: list[dict[str, int]],
    session_idx: int,
    total_sessions: int,
) -> dict[str, Any]:
    config = score_perf_csv.load_threshold_config(SLO_FILE)
    checks = score_perf_csv.evaluate(rows, profile, config)
    hard_fail = [check for check in checks if check.level == "hard" and not check.passed]
    advisory_fail = [check for check in checks if check.level == "advisory" and not check.passed]
    return {
        "profile": profile,
        "segment_selector": session_selector,
        "session_index": session_idx,
        "total_sessions": total_sessions,
        "rows": len(rows),
        "duration_s": score_perf_csv.duration_s(rows),
        "hard_failures": len(hard_fail),
        "advisory_warnings": len(advisory_fail),
        "result": "FAIL" if hard_fail else ("PASS_WITH_WARNINGS" if advisory_fail else "PASS"),
        "checks": [
            {
                "metric": check.metric,
                "level": check.level,
                "source": check.source,
                "value": check.value,
                "op": check.op,
                "limit": check.limit,
                "passed": check.passed,
            }
            for check in checks
        ],
    }


def _panic_path_for_csv(csv_path: Path) -> Path | None:
    candidate = csv_path.with_suffix(".panic.jsonl")
    return candidate if candidate.exists() else None


def _panic_summary(panic_path: Path | None) -> tuple[dict[str, Any], str]:
    if panic_path is None:
        return {
            "present": False,
            "runtime_crash_detected": False,
            "preexisting_crash_state": False,
            "state_change_count": None,
        }, "PASS"

    panic_kv, raw_panic_kv = import_drive_log.run_kv_parser(import_drive_log.SOAK_PARSE_PANIC, panic_path)
    runtime_crash = import_drive_log.panic_runtime_crash_detected(panic_kv)
    preexisting_crash = import_drive_log.panic_preexisting_crash_state(panic_kv)
    if runtime_crash:
        base_result = "FAIL"
    elif preexisting_crash:
        base_result = "PASS_WITH_WARNINGS"
    else:
        base_result = "PASS"
    return {
        "present": True,
        "runtime_crash_detected": runtime_crash,
        "preexisting_crash_state": preexisting_crash,
        "state_change_count": import_drive_log.integer(panic_kv.get("state_change_count", "")),
        "first_was_crash": import_drive_log.integer(panic_kv.get("first_was_crash", "")),
        "last_was_crash": import_drive_log.integer(panic_kv.get("last_was_crash", "")),
        "raw_kv": panic_kv,
        "path": str(panic_path),
        "parsed_text": raw_panic_kv,
    }, base_result


def coverage_status_for(unsupported_metrics: list[str]) -> str:
    unsupported = set(unsupported_metrics)
    if {"perf_drop_delta", "event_drop_delta"} & unsupported:
        return "partial_legacy_import"
    if unsupported:
        return "full_runtime_gates"
    return "full"


def append_import_sections(
    text_path: Path,
    *,
    csv_path: Path,
    source_schema: int,
    coverage_status: str,
    unsupported_metrics: list[str],
    selected_segment: dict[str, Any],
    peak_diagnostics: dict[str, dict[str, Any]],
    csv_scorecard: dict[str, Any],
    panic_summary: dict[str, Any],
) -> None:
    lines = [
        "",
        "## Imported CSV",
        "",
        f"- Source CSV: `{csv_path}`",
        f"- Source schema: `{source_schema}`",
        f"- Coverage status: `{coverage_status}`",
        f"- Selected segment: `{selected_segment['session_index']}`"
        + (f" token={selected_segment['token']}" if selected_segment.get("token") else ""),
        f"- Segment rows/duration: {selected_segment['row_count']} rows / {selected_segment['duration_s']:.1f}s",
        f"- Segment drive evidence: speed_rows={selected_segment['speed_active_rows']}, gps_rows={selected_segment['gps_valid_rows']}, rx_delta={selected_segment['rx_delta']}",
        f"- Unsupported metrics: {', '.join(unsupported_metrics) if unsupported_metrics else 'none'}",
        f"- Panic log: {'present' if panic_summary.get('present') else 'missing'} `{panic_summary.get('path', 'n/a')}`",
        f"- Panic runtime crash detected: {'yes' if panic_summary.get('runtime_crash_detected') else 'no'}",
        f"- Panic preexisting crash state: {'yes' if panic_summary.get('preexisting_crash_state') else 'no'}",
        "",
        "## Peak Diagnostics",
        "",
    ]
    if peak_diagnostics:
        for metric_name in sorted(peak_diagnostics):
            peak = peak_diagnostics[metric_name]
            lines.append(
                f"- `{metric_name}`: value={int(round(peak['value'])) if abs(peak['value'] - round(peak['value'])) < 1e-9 else peak['value']}, row={peak['row_index']}, millis={peak['millis']}"
            )
    else:
        lines.append("- none")
    lines.extend(
        [
            "",
            "## Complementary CSV Scorecard",
            "",
            f"- Profile: `{csv_scorecard['profile']}`",
            f"- Segment selector: `{csv_scorecard['segment_selector']}`",
            f"- Session: {csv_scorecard['session_index']}/{csv_scorecard['total_sessions']}",
            f"- Rows: {csv_scorecard['rows']}, Duration: {csv_scorecard['duration_s']:.1f}s",
            f"- CSV SLO result: **{csv_scorecard['result']}** (hard={csv_scorecard['hard_failures']}, advisory={csv_scorecard['advisory_warnings']})",
        ]
    )
    with text_path.open("a", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    csv_path = Path(args.input).resolve()
    if not csv_path.exists():
        print(f"ERROR: file not found: {csv_path}", file=sys.stderr)
        return 3

    selector = _selector_arg(args)
    try:
        sessions = load_sessions(csv_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3
    if not sessions:
        print("ERROR: no sessions found in CSV", file=sys.stderr)
        return 3

    try:
        session_meta, rows, selected_summary, summaries, effective_selector = select_segment(sessions, selector)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    if args.list_segments:
        print(render_segment_listing(summaries, selected_summary.session_index, effective_selector, csv_path), end="")
        return 0

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    source_schema = selected_summary.schema
    profile = args.profile
    suite_or_profile = profile
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    git_sha = args.git_sha or "unknown"
    git_ref = args.git_ref or "unknown"
    board_id = args.board_id or "unknown"
    stress_class = args.stress_class
    run_id = f"perf_csv_import_{timestamp}_{selected_summary.token or 'unknown'}"

    segments_payload = {
        "source": str(csv_path),
        "segment_selector": selector,
        "effective_segment_selector": effective_selector,
        "selected_segment": selected_summary.to_dict(),
        "sessions": [summary.to_dict() for summary in summaries],
    }
    segments_path = out_dir / "segments.json"
    segments_path.write_text(json.dumps(segments_payload, indent=2) + "\n", encoding="utf-8")

    metrics, peak_diagnostics, unsupported_metrics = extract_metrics(rows, source_schema)
    if not metrics:
        print("ERROR: no metrics extracted from CSV", file=sys.stderr)
        return 3

    metrics_ndjson = out_dir / "metrics.ndjson"
    write_metrics_ndjson(metrics_ndjson, run_id, git_sha, suite_or_profile, metrics)

    csv_scorecard = run_csv_scorecard(profile, effective_selector, rows, selected_summary.session_index, len(summaries))
    csv_scorecard_path = out_dir / "csv_scorecard.json"
    csv_scorecard_path.write_text(json.dumps(csv_scorecard, indent=2) + "\n", encoding="utf-8")

    panic_path = _panic_path_for_csv(csv_path)
    try:
        panic_summary, base_result = _panic_summary(panic_path)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3
    if panic_summary.get("present") and panic_summary.get("parsed_text"):
        (out_dir / "parsed_panic_kv.txt").write_text(str(panic_summary["parsed_text"]), encoding="utf-8")

    coverage_status = coverage_status_for(unsupported_metrics)
    selected_segment_payload = {
        **selected_summary.to_dict(),
        "selector": selector,
        "effective_selector": effective_selector,
    }
    diagnostics = {
        "source_files": {
            "csv": str(csv_path),
            "panic_jsonl": panic_summary.get("path", ""),
        },
        "source_schema": source_schema,
        "coverage_status": coverage_status,
        "unsupported_metrics": unsupported_metrics,
        "selected_segment": selected_segment_payload,
        "peaks": peak_diagnostics,
        "panic": {
            key: value
            for key, value in panic_summary.items()
            if key not in {"parsed_text", "raw_kv"}
        },
    }
    diagnostics_path = out_dir / "import_diagnostics.json"
    diagnostics_path.write_text(json.dumps(diagnostics, indent=2) + "\n", encoding="utf-8")

    manifest_path = out_dir / "manifest.json"
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "git_sha": git_sha,
        "git_ref": git_ref,
        "run_kind": "real_fw_soak",
        "board_id": board_id,
        "env": "perf-csv-import",
        "lane": args.lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "result": base_result,
        "base_result": base_result,
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
        "tracks": [suite_or_profile],
        "source_input": str(csv_path),
        "source_type": "perf_csv",
        "source_schema": source_schema,
        "selected_segment": selected_segment_payload,
        "unsupported_metrics": unsupported_metrics,
        "coverage_status": coverage_status,
        "segments_file": "segments.json",
        "csv_scorecard_file": "csv_scorecard.json",
        "import_diagnostics_file": "import_diagnostics.json",
        "rows": len(rows),
        "duration_s": selected_summary.duration_s,
    }
    if panic_summary.get("present"):
        manifest["source_panic_jsonl"] = str(panic_path)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    baseline_path = Path(args.compare_to).resolve() if args.compare_to else None
    try:
        scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_path)
    except Exception as exc:
        print(f"ERROR: scoring failed: {exc}", file=sys.stderr)
        return 3

    scoring_path = out_dir / "scoring.json"
    comparison_txt = out_dir / "comparison.txt"
    comparison_tsv = out_dir / "comparison.tsv"
    scoring_path.write_text(json.dumps(scored, indent=2) + "\n", encoding="utf-8")
    write_comparison_text(scored, comparison_txt)
    write_comparison_tsv(scored, comparison_tsv)
    append_import_sections(
        comparison_txt,
        csv_path=csv_path,
        source_schema=source_schema,
        coverage_status=coverage_status,
        unsupported_metrics=unsupported_metrics,
        selected_segment=selected_segment_payload,
        peak_diagnostics=peak_diagnostics,
        csv_scorecard=csv_scorecard,
        panic_summary=panic_summary,
    )

    manifest["result"] = scored["result"]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    summary = scored["summary"]
    print(
        f"Source: {csv_path.name} segment {selected_summary.session_index}/{len(summaries)} "
        f"({len(rows)} rows, {selected_summary.duration_s:.1f}s)"
    )
    print(f"Segment selector: {effective_selector}")
    print(f"Coverage status: {coverage_status}")
    print(
        f"Hardware catalog score: {scored['result']} "
        f"(hard={summary['hard_failures']}, advisory={summary['advisory_failures']}, unsupported={summary.get('unsupported_metrics', 0)})"
    )
    print(
        f"CSV SLO scorecard:      {csv_scorecard['result']} "
        f"(hard={csv_scorecard['hard_failures']}, advisory={csv_scorecard['advisory_warnings']})"
    )
    print(f"Artifacts: {out_dir}")

    result = str(scored["result"])
    if result == "FAIL":
        return 2
    if result == "PASS_WITH_WARNINGS":
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
