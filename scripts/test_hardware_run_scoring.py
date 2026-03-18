#!/usr/bin/env python3
"""Deterministic regression tests for hardware manifest scoring and extraction."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import score_hardware_run  # type: ignore  # noqa: E402


CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_metrics(path: Path, records: list[dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        for record in records:
            handle.write(json.dumps(record))
            handle.write("\n")


def write_manifest(
    path: Path,
    *,
    run_id: str,
    git_sha: str,
    git_ref: str,
    run_kind: str,
    board_id: str,
    env: str,
    lane: str,
    suite_or_profile: str,
    stress_class: str,
    result: str,
    metrics_file: str,
    scoring_file: str = "scoring.json",
    base_result: str | None = None,
    tracks: list[str] | None = None,
    unsupported_metrics: list[str] | None = None,
    source_type: str | None = None,
    source_schema: int | None = None,
    coverage_status: str | None = None,
) -> None:
    payload = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": "2026-03-12T00:00:00Z",
        "git_sha": git_sha,
        "git_ref": git_ref,
        "run_kind": run_kind,
        "board_id": board_id,
        "env": env,
        "lane": lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "result": result,
        "metrics_file": metrics_file,
        "scoring_file": scoring_file,
    }
    if base_result is not None:
        payload["base_result"] = base_result
    if tracks is not None:
        payload["tracks"] = tracks
    if unsupported_metrics is not None:
        payload["unsupported_metrics"] = unsupported_metrics
    if source_type is not None:
        payload["source_type"] = source_type
    if source_schema is not None:
        payload["source_schema"] = source_schema
    if coverage_status is not None:
        payload["coverage_status"] = coverage_status
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def device_metric(suite: str, metric: str, value: float) -> dict[str, object]:
    return {
        "schema_version": 1,
        "run_id": "run-1",
        "git_sha": "abc1234",
        "run_kind": "device_suite",
        "suite_or_profile": suite,
        "metric": metric,
        "sample": "value",
        "value": value,
        "unit": "count",
        "tags": {},
    }


def soak_metric(track: str, metric: str, value: float) -> dict[str, object]:
    return {
        "schema_version": 1,
        "run_id": "soak-1",
        "git_sha": "abc1234",
        "run_kind": "real_fw_soak",
        "suite_or_profile": track,
        "metric": metric,
        "sample": "value",
        "value": value,
        "unit": "count",
        "tags": {},
    }


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "score_hardware_run.py"), *args],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_no_baseline_and_run_variance_commit_regression(tmpdir: Path) -> None:
    case_dir = tmpdir / "no_baseline"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    baseline_same_path = case_dir / "baseline_same.json"
    baseline_diff_path = case_dir / "baseline_diff.json"

    write_metrics(
        metrics_path,
        [
            device_metric("test_device_heap", "baseline_internal_free_bytes", 140000),
            device_metric("test_device_heap", "baseline_internal_largest_block_bytes", 60000),
            device_metric("test_device_heap", "internal_alloc_recovery_delta_bytes", 32),
            device_metric("test_device_heap", "spiram_alloc_recovery_delta_bytes", 64)
        ],
    )
    write_manifest(
        manifest_path,
        run_id="run-1",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )

    no_baseline = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(no_baseline["result"] == "NO_BASELINE", "missing baseline should return NO_BASELINE")
    assert_true(no_baseline["comparison_kind"] == "no_baseline", "missing baseline should not classify as regression")

    write_manifest(
        baseline_same_path,
        run_id="run-0",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    same_sha = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_same_path)
    assert_true(same_sha["comparison_kind"] == "run_variance", "same git sha must classify as run_variance")

    write_manifest(
        baseline_diff_path,
        run_id="run-base",
        git_sha="def5678",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    diff_sha = score_hardware_run.score_run(manifest_path, CATALOG_PATH, baseline_diff_path)
    assert_true(diff_sha["comparison_kind"] == "commit_regression", "different git sha must classify as commit regression")


def test_selector_and_track_matching(tmpdir: Path) -> None:
    case_dir = tmpdir / "selector_matching"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    write_metrics(
        metrics_path,
        [
            soak_metric("drive_wifi_ap", "metrics_ok_samples", 10),
            soak_metric("drive_wifi_ap", "rx_packets_delta", 120),
            soak_metric("drive_wifi_ap", "parse_successes_delta", 120),
            soak_metric("drive_wifi_ap", "parse_failures_delta", 0),
            soak_metric("drive_wifi_ap", "queue_drops_delta", 0),
            soak_metric("drive_wifi_ap", "perf_drop_delta", 0),
            soak_metric("drive_wifi_ap", "event_drop_delta", 0),
            soak_metric("drive_wifi_ap", "oversize_drops_delta", 0),
            soak_metric("drive_wifi_ap", "loop_max_peak_us", 200000),
            soak_metric("drive_wifi_ap", "flush_max_peak_us", 30000),
            soak_metric("drive_wifi_ap", "wifi_max_peak_us", 3000),
            soak_metric("drive_wifi_ap", "ble_drain_max_peak_us", 5000),
            soak_metric("drive_wifi_ap", "sd_max_peak_us", 10000),
            soak_metric("drive_wifi_ap", "fs_max_peak_us", 10000),
            soak_metric("drive_wifi_ap", "queue_high_water_peak", 4),
            soak_metric("drive_wifi_ap", "wifi_connect_deferred_delta", 1),
            soak_metric("drive_wifi_ap", "dma_free_min_bytes", 25000),
            soak_metric("drive_wifi_ap", "dma_largest_min_bytes", 14000),
            soak_metric("drive_wifi_ap", "ble_process_max_peak_us", 40000),
            soak_metric("drive_wifi_ap", "disp_pipe_max_peak_us", 30000),
            soak_metric("drive_wifi_ap", "ble_mutex_timeout_delta", 0),
            soak_metric("drive_wifi_ap", "display_skips_delta", 0),
            soak_metric("drive_wifi_ap", "reconnects_delta", 0),
            soak_metric("drive_wifi_ap", "disconnects_delta", 0),
            soak_metric("drive_wifi_ap", "gps_obs_drops_delta", 0),
            soak_metric("drive_wifi_ap", "wifi_p95_us", 1800),
            soak_metric("drive_wifi_ap", "disp_pipe_p95_us", 21000),
            soak_metric("drive_wifi_ap", "dma_fragmentation_pct_p95", 18)
        ],
    )
    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="soak-base",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="radio",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    assert_true(result["comparison_kind"] == "no_baseline", "board mismatch must skip baseline comparison")


def test_drive_wifi_ap_loop_peak_is_informational(tmpdir: Path) -> None:
    case_dir = tmpdir / "wifi_ap_loop_info"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    current_manifest = case_dir / "current.json"
    baseline_manifest = case_dir / "baseline.json"

    current_records = [
        soak_metric("drive_wifi_ap", "metrics_ok_samples", 10),
        soak_metric("drive_wifi_ap", "rx_packets_delta", 120),
        soak_metric("drive_wifi_ap", "parse_successes_delta", 120),
        soak_metric("drive_wifi_ap", "parse_failures_delta", 0),
        soak_metric("drive_wifi_ap", "queue_drops_delta", 0),
        soak_metric("drive_wifi_ap", "perf_drop_delta", 0),
        soak_metric("drive_wifi_ap", "event_drop_delta", 0),
        soak_metric("drive_wifi_ap", "oversize_drops_delta", 0),
        soak_metric("drive_wifi_ap", "loop_max_peak_us", 220000),
        soak_metric("drive_wifi_ap", "flush_max_peak_us", 30000),
        soak_metric("drive_wifi_ap", "wifi_max_peak_us", 3000),
        soak_metric("drive_wifi_ap", "ble_drain_max_peak_us", 5000),
        soak_metric("drive_wifi_ap", "sd_max_peak_us", 10000),
        soak_metric("drive_wifi_ap", "fs_max_peak_us", 10000),
        soak_metric("drive_wifi_ap", "queue_high_water_peak", 4),
        soak_metric("drive_wifi_ap", "wifi_connect_deferred_delta", 1),
        soak_metric("drive_wifi_ap", "dma_free_min_bytes", 25000),
        soak_metric("drive_wifi_ap", "dma_largest_min_bytes", 14000),
        soak_metric("drive_wifi_ap", "ble_process_max_peak_us", 40000),
        soak_metric("drive_wifi_ap", "disp_pipe_max_peak_us", 30000),
        soak_metric("drive_wifi_ap", "ble_mutex_timeout_delta", 0),
        soak_metric("drive_wifi_ap", "display_skips_delta", 0),
        soak_metric("drive_wifi_ap", "reconnects_delta", 0),
        soak_metric("drive_wifi_ap", "disconnects_delta", 0),
        soak_metric("drive_wifi_ap", "gps_obs_drops_delta", 0),
        soak_metric("drive_wifi_ap", "wifi_p95_us", 1800),
        soak_metric("drive_wifi_ap", "disp_pipe_p95_us", 21000),
        soak_metric("drive_wifi_ap", "dma_fragmentation_pct_p95", 18),
    ]
    baseline_records = [dict(item) for item in current_records]
    for record in baseline_records:
        if record["metric"] == "loop_max_peak_us":
            record["value"] = 180000
            break

    write_metrics(metrics_path, current_records)
    baseline_metrics_path = case_dir / "baseline.ndjson"
    write_metrics(baseline_metrics_path, baseline_records)

    write_manifest(
        current_manifest,
        run_id="soak-current",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )
    write_manifest(
        baseline_manifest,
        run_id="soak-base",
        git_sha="base567",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="real-fw-soak",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="baseline.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(current_manifest, CATALOG_PATH, baseline_manifest)
    assert_true(result["result"] == "PASS", f"wifi-ap loop peak should not fail run: {result}")
    loop_metric = next(metric for metric in result["metrics"] if metric["metric"] == "loop_max_peak_us")
    assert_true(loop_metric["score_level"] == "info", f"wifi-ap loop peak should be info: {loop_metric}")
    assert_true(loop_metric["score_status"] == "info", f"wifi-ap loop regression should stay informational: {loop_metric}")


def test_optional_metric_gap_warns_but_is_not_inconclusive(tmpdir: Path) -> None:
    case_dir = tmpdir / "inconclusive"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    catalog_path = case_dir / "catalog.json"

    write_metrics(
        metrics_path,
        [
            {
                "schema_version": 1,
                "run_id": "custom-run",
                "git_sha": "abc1234",
                "run_kind": "custom_kind",
                "suite_or_profile": "track-a",
                "metric": "required_metric",
                "sample": "value",
                "value": 10,
                "unit": "count",
                "tags": {},
            }
        ],
    )
    write_manifest(
        manifest_path,
        run_id="custom-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="custom_kind",
        board_id="release",
        env="custom",
        lane="custom",
        suite_or_profile="track-a",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["track-a"],
    )
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "metrics": [
                    {
                        "metric": "required_metric",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "higher_better",
                        "score_level": "hard",
                        "required": True,
                        "absolute_min": 1,
                        "absolute_max": None,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                    {
                        "metric": "optional_metric",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "higher_better",
                        "score_level": "advisory",
                        "required": False,
                        "absolute_min": None,
                        "absolute_max": None,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    result = score_hardware_run.score_run(manifest_path, catalog_path)
    assert_true(result["result"] == "PASS_WITH_WARNINGS", "missing optional advisory metrics should warn, not become inconclusive")


def test_unsupported_metrics_do_not_fail_run(tmpdir: Path) -> None:
    case_dir = tmpdir / "unsupported_metrics"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    catalog_path = case_dir / "catalog.json"

    write_metrics(
        metrics_path,
        [
            {
                "schema_version": 1,
                "run_id": "custom-run",
                "git_sha": "abc1234",
                "run_kind": "custom_kind",
                "suite_or_profile": "track-a",
                "metric": "required_metric",
                "sample": "value",
                "value": 10,
                "unit": "count",
                "tags": {},
            }
        ],
    )
    write_manifest(
        manifest_path,
        run_id="custom-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="custom_kind",
        board_id="release",
        env="custom",
        lane="custom",
        suite_or_profile="track-a",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["track-a"],
        unsupported_metrics=["required_unsupported"],
        source_type="perf_csv",
        source_schema=12,
        coverage_status="partial_legacy_import",
    )
    catalog_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "metrics": [
                    {
                        "metric": "required_metric",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "higher_better",
                        "score_level": "hard",
                        "required": True,
                        "absolute_min": 1,
                        "absolute_max": None,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                    {
                        "metric": "required_unsupported",
                        "run_kind": "custom_kind",
                        "selector": {"suite_or_profile": "track-a"},
                        "unit": "count",
                        "aggregation": "last",
                        "direction": "lower_better",
                        "score_level": "hard",
                        "required": True,
                        "absolute_min": 0,
                        "absolute_max": 0,
                        "regress_abs": None,
                        "regress_pct": None,
                    },
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    result = score_hardware_run.score_run(manifest_path, catalog_path)
    assert_true(result["result"] == "NO_BASELINE", f"unsupported required metrics must not fail the run: {result}")
    unsupported = [metric for metric in result["metrics"] if metric["classification"] == "unsupported"]
    assert_true(len(unsupported) == 1, f"expected one unsupported metric: {result}")
    assert_true(result["summary"]["unsupported_metrics"] == 1, f"summary should count unsupported metrics: {result}")


def test_uncataloged_metric_rejected(tmpdir: Path) -> None:
    case_dir = tmpdir / "uncataloged"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    write_metrics(
        metrics_path,
        [device_metric("test_device_heap", "uncataloged_metric", 1)],
    )
    write_manifest(
        manifest_path,
        run_id="bad-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    result = run_cli(str(manifest_path), "--catalog", str(CATALOG_PATH))
    assert_true(result.returncode == 3, "uncataloged emitted metrics must fail scoring setup")


def test_connect_burst_metrics_are_cataloged(tmpdir: Path) -> None:
    case_dir = tmpdir / "connect_burst_cataloged"
    case_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"

    write_metrics(
        metrics_path,
        [
            soak_metric("drive_wifi_ap", "metrics_ok_samples", 10),
            soak_metric("drive_wifi_ap", "rx_packets_delta", 100),
            soak_metric("drive_wifi_ap", "parse_successes_delta", 100),
            soak_metric("drive_wifi_ap", "parse_failures_delta", 0),
            soak_metric("drive_wifi_ap", "queue_drops_delta", 0),
            soak_metric("drive_wifi_ap", "perf_drop_delta", 0),
            soak_metric("drive_wifi_ap", "event_drop_delta", 0),
            soak_metric("drive_wifi_ap", "oversize_drops_delta", 0),
            soak_metric("drive_wifi_ap", "flush_max_peak_us", 30000),
            soak_metric("drive_wifi_ap", "loop_max_peak_us", 70000),
            soak_metric("drive_wifi_ap", "wifi_max_peak_us", 2500),
            soak_metric("drive_wifi_ap", "ble_drain_max_peak_us", 5000),
            soak_metric("drive_wifi_ap", "sd_max_peak_us", 12000),
            soak_metric("drive_wifi_ap", "fs_max_peak_us", 0),
            soak_metric("drive_wifi_ap", "queue_high_water_peak", 4),
            soak_metric("drive_wifi_ap", "wifi_connect_deferred_delta", 0),
            soak_metric("drive_wifi_ap", "dma_free_min_bytes", 25000),
            soak_metric("drive_wifi_ap", "dma_largest_min_bytes", 14000),
            soak_metric("drive_wifi_ap", "ble_process_max_peak_us", 200),
            soak_metric("drive_wifi_ap", "disp_pipe_max_peak_us", 50000),
            soak_metric("drive_wifi_ap", "ble_mutex_timeout_delta", 0),
            soak_metric("drive_wifi_ap", "display_updates_delta", 200),
            soak_metric("drive_wifi_ap", "display_skips_delta", 0),
            soak_metric("drive_wifi_ap", "reconnects_delta", 0),
            soak_metric("drive_wifi_ap", "disconnects_delta", 0),
            soak_metric("drive_wifi_ap", "gps_obs_drops_delta", 0),
            soak_metric("drive_wifi_ap", "wifi_p95_us", 1800),
            soak_metric("drive_wifi_ap", "disp_pipe_p95_us", 35000),
            soak_metric("drive_wifi_ap", "dma_fragmentation_pct_p95", 18),
            soak_metric("drive_wifi_ap", "samples_to_stable", 2),
            soak_metric("drive_wifi_ap", "time_to_stable_ms", 500),
            soak_metric("drive_wifi_ap", "connect_burst_samples_to_stable", 3),
            soak_metric("drive_wifi_ap", "connect_burst_time_to_stable_ms", 1200),
            soak_metric("drive_wifi_ap", "connect_burst_pre_ble_process_peak_us", 150),
            soak_metric("drive_wifi_ap", "connect_burst_pre_disp_pipe_peak_us", 48000),
            soak_metric("drive_wifi_ap", "connect_burst_ble_followup_request_alert_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_ble_followup_request_version_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_ble_connect_stable_callback_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_ble_proxy_start_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_disp_render_peak_us", 47000),
            soak_metric("drive_wifi_ap", "connect_burst_display_voice_peak_us", 0),
            soak_metric("drive_wifi_ap", "connect_burst_display_gap_recover_peak_us", 0),
        ],
    )

    write_manifest(
        manifest_path,
        run_id="soak-connect-burst",
        git_sha="abc1234",
        git_ref="main",
        run_kind="real_fw_soak",
        board_id="release",
        env="waveshare-349",
        lane="qualification",
        suite_or_profile="drive_wifi_ap",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["drive_wifi_ap"],
    )

    result = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(result["result"] == "NO_BASELINE", f"cataloged connect-burst metrics should score cleanly: {result}")
    metrics = {item["metric"] for item in result["metrics"]}
    assert_true("connect_burst_samples_to_stable" in metrics, f"missing connect-burst metric in score output: {result}")
    assert_true("connect_burst_disp_render_peak_us" in metrics, f"missing render peak metric in score output: {result}")


def test_extract_device_metrics_smoke(tmpdir: Path) -> None:
    case_dir = tmpdir / "extract_smoke"
    case_dir.mkdir(parents=True, exist_ok=True)
    log_path = case_dir / "suite.log"
    out_path = case_dir / "metrics.ndjson"
    manifest_path = case_dir / "manifest.json"
    log_path.write_text(
        "\n".join(
            [
                "[platformio] noise",
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "baseline_internal_free_bytes",
                        "sample": "baseline",
                        "value": 140000,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "baseline_internal_largest_block_bytes",
                        "sample": "baseline",
                        "value": 60000,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "internal_alloc_recovery_delta_bytes",
                        "sample": "recovery",
                        "value": 16,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
                json.dumps(
                    {
                        "schema_version": 1,
                        "run_id": "",
                        "git_sha": "",
                        "run_kind": "device_suite",
                        "suite_or_profile": "test_device_heap",
                        "metric": "spiram_alloc_recovery_delta_bytes",
                        "sample": "recovery",
                        "value": 32,
                        "unit": "bytes",
                        "tags": {},
                    }
                ),
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    extract = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "extract_device_metrics.py"),
            str(log_path),
            str(out_path),
            "--run-id",
            "suite-run",
            "--git-sha",
            "abc1234",
            "--suite",
            "test_device_heap",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(extract.returncode == 0, f"extract_device_metrics.py failed: {extract.stderr}")
    assert_true(out_path.read_text(encoding="utf-8").count("\n") == 4, "extractor must retain all structured metric lines")

    write_manifest(
        manifest_path,
        run_id="suite-run",
        git_sha="abc1234",
        git_ref="main",
        run_kind="device_suite",
        board_id="release",
        env="device",
        lane="device-tests",
        suite_or_profile="device_suite_collection",
        stress_class="core",
        result="PASS",
        metrics_file="metrics.ndjson",
        base_result="PASS",
        tracks=["test_device_heap"],
    )
    scored = score_hardware_run.score_run(manifest_path, CATALOG_PATH)
    assert_true(scored["summary"]["metrics_scored"] >= 4, "extracted metrics should feed the scorer")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="hardware-run-scoring-") as tmp:
        tmpdir = Path(tmp)
        test_no_baseline_and_run_variance_commit_regression(tmpdir)
        test_selector_and_track_matching(tmpdir)
        test_optional_metric_gap_warns_but_is_not_inconclusive(tmpdir)
        test_unsupported_metrics_do_not_fail_run(tmpdir)
        test_uncataloged_metric_rejected(tmpdir)
        test_connect_burst_metrics_are_cataloged(tmpdir)
        test_extract_device_metrics_smoke(tmpdir)
    print("hardware run scoring tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
