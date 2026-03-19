#!/usr/bin/env python3
"""Regression tests for drive-log metric rendering aliases and canonical keys."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import import_drive_log  # type: ignore  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def render_metrics(kv: dict[str, str]) -> set[str]:
    with tempfile.TemporaryDirectory() as tmp_dir:
        out_path = Path(tmp_dir) / "metrics.ndjson"
        count = import_drive_log.render_metrics_ndjson(
            out_path,
            "run_id",
            "deadbee",
            "drive_wifi_ap",
            kv,
        )
        rows = [
            json.loads(line)
            for line in out_path.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
    assert_true(count == len(rows), f"count mismatch: {count} vs {len(rows)}")
    return {row["metric"] for row in rows}


def test_render_metrics_ndjson_accepts_canonical_soak_keys() -> None:
    metrics = render_metrics(
        {
            "metrics_ok_samples": "12",
            "event_drop_delta": "0",
            "flush_max_peak_us": "21000",
            "loop_max_peak_us": "61000",
            "wifi_max_peak_us": "2400",
            "sd_max_peak_us": "9000",
            "ble_process_max_peak_us": "300",
            "disp_pipe_max_peak_us": "27000",
        }
    )
    expected = {
        "metrics_ok_samples",
        "event_drop_delta",
        "flush_max_peak_us",
        "loop_max_peak_us",
        "wifi_max_peak_us",
        "sd_max_peak_us",
        "ble_process_max_peak_us",
        "disp_pipe_max_peak_us",
    }
    assert_true(expected <= metrics, f"canonical metrics missing: {expected - metrics}")


def test_render_metrics_ndjson_still_accepts_legacy_alias_keys() -> None:
    metrics = render_metrics(
        {
            "ok_samples": "12",
            "event_drop_delta": "0",
            "flush_max_peak": "21000",
            "loop_max_peak": "61000",
            "wifi_max_peak": "2400",
            "sd_max_peak": "9000",
            "ble_process_max_peak": "300",
            "disp_pipe_max_peak": "27000",
        }
    )
    expected = {
        "metrics_ok_samples",
        "event_drop_delta",
        "flush_max_peak_us",
        "loop_max_peak_us",
        "wifi_max_peak_us",
        "sd_max_peak_us",
        "ble_process_max_peak_us",
        "disp_pipe_max_peak_us",
    }
    assert_true(expected <= metrics, f"alias metrics missing: {expected - metrics}")


def main() -> int:
    test_render_metrics_ndjson_accepts_canonical_soak_keys()
    test_render_metrics_ndjson_still_accepts_legacy_alias_keys()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
