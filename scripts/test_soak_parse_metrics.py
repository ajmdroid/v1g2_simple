#!/usr/bin/env python3
"""Regression tests for soak_parse_metrics connect-burst diagnostics."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "soak_parse_metrics.py"
FIXTURE = ROOT / "test" / "fixtures" / "perf" / "core_soak_connect_burst_reduced.metrics.jsonl"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_metrics(path: Path, *extra_args: str) -> dict[str, str]:
    completed = subprocess.run(
        [sys.executable, str(SCRIPT), str(path), *extra_args],
        capture_output=True,
        text=True,
        check=False,
    )
    assert_true(completed.returncode == 0, f"parser failed: {completed.stdout}\n{completed.stderr}")
    result: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key] = value
    return result


def write_metrics_jsonl(path: Path) -> None:
    start = datetime(2026, 3, 18, 11, 29, 37, tzinfo=timezone.utc)
    rows = [
        {
            "ts": start.isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTING_WAIT",
                "bleStateCode": 4,
                "subscribeStep": "GET_SERVICE",
                "subscribeStepCode": 0,
                "proxyAdvertising": 0,
                "bleProcessMaxUs": 900,
                "dispPipeMaxUs": 0,
                "dispMaxUs": 0,
                "displayVoiceMaxUs": 0,
                "displayGapRecoverMaxUs": 0,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
            },
        },
        {
            "ts": (start + timedelta(seconds=1)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 62000,
                "dispPipeMaxUs": 71000,
                "dispMaxUs": 44000,
                "displayVoiceMaxUs": 18000,
                "displayGapRecoverMaxUs": 3000,
                "bleFollowupRequestAlertMaxUs": 8000,
                "bleFollowupRequestVersionMaxUs": 17000,
                "bleConnectStableCallbackMaxUs": 21000,
                "bleProxyStartMaxUs": 58000,
            },
        },
        {
            "ts": (start + timedelta(seconds=2)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 12000,
                "dispPipeMaxUs": 32000,
                "dispMaxUs": 15000,
                "displayVoiceMaxUs": 0,
                "displayGapRecoverMaxUs": 1200,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
            },
        },
        {
            "ts": (start + timedelta(seconds=3)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 14000,
                "dispPipeMaxUs": 28000,
                "dispMaxUs": 11000,
                "displayVoiceMaxUs": 0,
                "displayGapRecoverMaxUs": 800,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
            },
        },
        {
            "ts": (start + timedelta(seconds=4)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "bleState": "CONNECTED",
                "bleStateCode": 8,
                "subscribeStep": "COMPLETE",
                "subscribeStepCode": 11,
                "proxyAdvertising": 1,
                "bleProcessMaxUs": 9000,
                "dispPipeMaxUs": 12000,
                "dispMaxUs": 6000,
                "displayVoiceMaxUs": 0,
                "displayGapRecoverMaxUs": 0,
                "bleFollowupRequestAlertMaxUs": 0,
                "bleFollowupRequestVersionMaxUs": 0,
                "bleConnectStableCallbackMaxUs": 0,
                "bleProxyStartMaxUs": 0,
            },
        },
    ]
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def test_reduced_fixture_surfaces_unstable_connect_burst() -> None:
    parsed = parse_metrics(
        FIXTURE,
        "--ble-threshold",
        "25000",
        "--connect-burst-disp-threshold",
        "50000",
        "--connect-burst-consecutive-samples",
        "3",
    )
    assert_true(parsed["connect_burst_detected"] == "1", f"burst not detected: {parsed}")
    assert_true(parsed["connect_burst_stabilized"] == "0", f"fixture should remain unstable: {parsed}")
    assert_true(parsed["connect_burst_event_ble_state"] == "CONNECTED", f"wrong event state: {parsed}")
    assert_true(parsed["connect_burst_event_subscribe_step"] == "COMPLETE", f"wrong subscribe step: {parsed}")
    assert_true(parsed["connect_burst_event_proxy_advertising"] == "1", f"wrong proxy flag: {parsed}")
    assert_true(parsed["connect_burst_pre_ble_process_peak"] == "73755", f"wrong ble peak: {parsed}")
    assert_true(parsed["connect_burst_pre_disp_pipe_peak"] == "73276", f"wrong display peak: {parsed}")
    assert_true(parsed["connect_burst_samples_to_stable"] == "", f"unexpected settle result: {parsed}")
    assert_true(parsed["connect_burst_time_to_stable_ms"] == "", f"unexpected settle time: {parsed}")


def test_synthetic_fixture_tracks_root_causes_and_settle_window() -> None:
    with tempfile.TemporaryDirectory() as tmp_dir:
        path = Path(tmp_dir) / "metrics.jsonl"
        write_metrics_jsonl(path)
        parsed = parse_metrics(
            path,
            "--ble-threshold",
            "25000",
            "--connect-burst-disp-threshold",
            "50000",
            "--connect-burst-consecutive-samples",
            "3",
        )

    assert_true(parsed["connect_burst_detected"] == "1", f"burst not detected: {parsed}")
    assert_true(parsed["connect_burst_stabilized"] == "1", f"burst should stabilize: {parsed}")
    assert_true(parsed["connect_burst_event_index"] == "1", f"wrong event index: {parsed}")
    assert_true(parsed["connect_burst_stable_index"] == "4", f"wrong stable index: {parsed}")
    assert_true(parsed["connect_burst_samples_to_stable"] == "4", f"wrong samples-to-stable: {parsed}")
    assert_true(parsed["connect_burst_time_to_stable_ms"] == "3000", f"wrong time-to-stable: {parsed}")
    assert_true(parsed["connect_burst_pre_ble_process_peak"] == "62000", f"wrong ble peak: {parsed}")
    assert_true(parsed["connect_burst_pre_disp_pipe_peak"] == "71000", f"wrong disp peak: {parsed}")
    assert_true(parsed["connect_burst_ble_followup_request_alert_peak"] == "8000", f"wrong alert peak: {parsed}")
    assert_true(parsed["connect_burst_ble_followup_request_version_peak"] == "17000", f"wrong version peak: {parsed}")
    assert_true(parsed["connect_burst_ble_connect_stable_callback_peak"] == "21000", f"wrong stable callback peak: {parsed}")
    assert_true(parsed["connect_burst_ble_proxy_start_peak"] == "58000", f"wrong proxy peak: {parsed}")
    assert_true(parsed["connect_burst_disp_render_peak"] == "44000", f"wrong render peak: {parsed}")
    assert_true(parsed["connect_burst_display_voice_peak"] == "18000", f"wrong voice peak: {parsed}")
    assert_true(parsed["connect_burst_display_gap_recover_peak"] == "3000", f"wrong gap-recover peak: {parsed}")


def main() -> int:
    test_reduced_fixture_surfaces_unstable_connect_burst()
    test_synthetic_fixture_tracks_root_causes_and_settle_window()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
