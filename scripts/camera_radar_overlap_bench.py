#!/usr/bin/env python3
"""Real-runtime camera+radar overlap bench runner.

Uses GPS scaffold injection to drive the real CameraAlertModule pipeline
while a V1 BLE scenario provides live radar alerts.  This exercises the
full camera detection state machine (IDLE -> DETECTED -> APPROACHING ->
CONFIRMED) via road_map.bin, unlike the debug-render-only stress runner.

Prerequisites:
  - Device has test/fixtures/camera_types_road_map.bin on SD at /road_map.bin
  - Device Wi-Fi reachable at the configured base URL
  - Camera alerts enabled in device settings
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import sys
import time
import urllib.parse
import urllib.request

# Camera fixture location (from generate_camera_fixture.py)
CAMERA_LAT = 39.7460
CAMERA_LON = -104.9905

# Northbound approach trace: start ~750 m south, straight north
APPROACH_START_LAT = 39.7392
APPROACH_LON = CAMERA_LON + 0.0002  # slight east offset to stay in corridor

# Detection constants mirroring camera_alert_module.cpp
CAMERA_POLL_INTERVAL_MS = 500
CAMERA_MIN_SPEED_MPH = 15.0
STEP_INTERVAL_S = 0.6  # slightly above poll interval


class BenchFailure(RuntimeError):
    pass


def request_json(
    base_url: str,
    path: str,
    timeout_seconds: int,
    method: str = "GET",
    json_body: dict | None = None,
) -> dict:
    data = None
    headers: dict[str, str] = {}
    if json_body is not None:
        data = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(
        base_url + path,
        data=data,
        method=method,
        headers=headers,
    )
    with urllib.request.urlopen(req, timeout=timeout_seconds) as resp:
        body = resp.read().decode("utf-8")
    return json.loads(body)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BenchFailure(message)


def metric_int(metrics: dict, key: str) -> int:
    return int(metrics.get(key, 0) or 0)


def metric_delta(before: dict, after: dict, key: str) -> int:
    return metric_int(after, key) - metric_int(before, key)


def derive_base_url(metrics_url: str) -> str:
    marker = "/api/debug/metrics"
    idx = metrics_url.find(marker)
    if idx >= 0:
        return metrics_url[:idx].rstrip("/")
    return metrics_url.rstrip("/")


def inject_gps(base_url: str, lat: float, lon: float, speed_mph: float,
               timeout_s: int) -> dict:
    return request_json(base_url, "/api/gps/config", timeout_s,
                        method="POST",
                        json_body={
                            "enabled": True,
                            "latitude": lat,
                            "longitude": lon,
                            "speedMph": speed_mph,
                            "hasFix": True,
                            "satellites": 12,
                            "hdop": 1.0,
                        })


def camera_status(base_url: str, timeout_s: int) -> dict:
    return request_json(base_url, "/api/cameras/status", timeout_s)


def build_approach_trace(steps: int) -> list[tuple[float, float]]:
    """Generate GPS positions approaching camera from south."""
    lat_start = APPROACH_START_LAT
    lat_end = CAMERA_LAT + 0.0010  # overshoot slightly past camera
    trace = []
    for i in range(steps):
        frac = i / max(1, steps - 1)
        lat = lat_start + (lat_end - lat_start) * frac
        trace.append((lat, APPROACH_LON))
    return trace


def run_approach(
    base_url: str,
    timeout_s: int,
    speed_mph: float,
    trace: list[tuple[float, float]],
    scenario_active: bool,
) -> dict:
    """Drive the GPS trace and track camera detection transitions."""
    checks: list[str] = []
    saw_display_active = False
    display_active_step = -1
    display_active_distance: int | None = None

    for step_idx, (lat, lon) in enumerate(trace):
        inject_gps(base_url, lat, lon, speed_mph, timeout_s)
        time.sleep(STEP_INTERVAL_S)

        status = camera_status(base_url, timeout_s)
        active = status.get("displayActive", False)
        dist = status.get("distanceCm")

        if active and not saw_display_active:
            saw_display_active = True
            display_active_step = step_idx
            display_active_distance = dist
            checks.append(
                f"camera displayActive at step {step_idx}/{len(trace)} "
                f"dist={dist}cm lat={lat:.5f}"
            )

    if saw_display_active:
        checks.append("camera detection confirmed via real pipeline")
    else:
        checks.append("FAIL: camera never reached displayActive")

    return {
        "saw_display_active": saw_display_active,
        "display_active_step": display_active_step,
        "display_active_distance": display_active_distance,
        "total_steps": len(trace),
        "checks": checks,
        "scenario_active": scenario_active,
    }


def run_bench(base_url: str, args: argparse.Namespace) -> dict:
    """Run the full bench: camera-only, then camera+radar overlap."""
    timeout_s = args.http_timeout_seconds
    speed_mph = args.speed_mph
    trace_steps = args.trace_steps
    trace = build_approach_trace(trace_steps)

    results: dict = {
        "config": {
            "base_url": base_url,
            "speed_mph": speed_mph,
            "trace_steps": trace_steps,
            "step_interval_s": STEP_INTERVAL_S,
            "camera_lat": CAMERA_LAT,
            "camera_lon": CAMERA_LON,
            "approach_start_lat": APPROACH_START_LAT,
            "scenario_id": args.scenario_id,
        },
        "phases": [],
    }

    # --- Precheck ---
    pre_metrics = request_json(base_url, "/api/debug/metrics", timeout_s)
    cam_status = camera_status(base_url, timeout_s)
    camera_count = cam_status.get("cameraCount", 0)
    require(camera_count > 0,
            f"precheck: road_map.bin not loaded or empty (cameraCount={camera_count})")
    print(f"precheck: cameraCount={camera_count}")

    # Enable camera alerts
    request_json(base_url, "/api/cameras/settings", timeout_s,
                 method="POST",
                 json_body={"cameraAlertsEnabled": True})

    # --- Phase 1: Camera-only approach (no radar) ---
    print("phase 1: camera-only approach")
    # Stop any running scenario
    try:
        request_json(base_url, "/api/debug/v1-scenario/stop", timeout_s,
                     method="POST", json_body={})
    except Exception:
        pass

    phase1 = run_approach(base_url, timeout_s, speed_mph, trace,
                          scenario_active=False)
    phase1["phase"] = "camera_only"
    results["phases"].append(phase1)

    # Reset camera encounter for next phase
    time.sleep(1.0)
    inject_gps(base_url, APPROACH_START_LAT - 0.01, APPROACH_LON, 0.0, timeout_s)
    time.sleep(1.5)  # let encounter expire

    # --- Phase 2: Camera + radar overlap ---
    print("phase 2: camera+radar overlap")
    scenario_started = False
    try:
        start_resp = request_json(
            base_url, "/api/debug/v1-scenario/start", timeout_s,
            method="POST",
            json_body={
                "id": args.scenario_id,
                "durationScalePct": args.duration_scale_pct,
            })
        scenario_started = bool(start_resp.get("success"))
        if scenario_started:
            print(f"  scenario '{args.scenario_id}' started")
        else:
            print(f"  WARNING: scenario start returned success=false")
    except Exception as exc:
        print(f"  WARNING: scenario start failed: {exc}")

    phase2 = run_approach(base_url, timeout_s, speed_mph, trace,
                          scenario_active=scenario_started)
    phase2["phase"] = "camera_radar_overlap"
    results["phases"].append(phase2)

    # Cleanup: stop scenario
    try:
        request_json(base_url, "/api/debug/v1-scenario/stop", timeout_s,
                     method="POST", json_body={})
    except Exception:
        pass

    # Cleanup: disable GPS scaffold
    try:
        inject_gps(base_url, 0.0, 0.0, 0.0, timeout_s)
    except Exception:
        pass

    # --- Collect post-metrics ---
    post_metrics = request_json(base_url, "/api/debug/metrics", timeout_s)

    results["metrics"] = {
        "displayUpdates_delta": metric_delta(pre_metrics, post_metrics,
                                             "displayUpdates"),
        "cameraDisplayFrames_delta": metric_delta(pre_metrics, post_metrics,
                                                  "cameraDisplayFrames"),
        "cameraProcessMaxUs": metric_int(post_metrics, "cameraProcessMaxUs"),
        "cameraDisplayMaxUs": metric_int(post_metrics, "cameraDisplayMaxUs"),
        "dispPipeMaxUs": metric_int(post_metrics, "dispPipeMaxUs"),
        "parseFailures_delta": metric_delta(pre_metrics, post_metrics,
                                            "parseFailures"),
        "queueDrops_delta": metric_delta(pre_metrics, post_metrics,
                                         "queueDrops"),
    }

    # --- Verdict ---
    phase1_ok = phase1["saw_display_active"]
    phase2_ok = phase2["saw_display_active"]
    all_checks: list[str] = []

    if phase1_ok:
        all_checks.append("PASS: camera-only approach detected camera")
    else:
        all_checks.append("FAIL: camera-only approach did NOT detect camera")

    if phase2_ok:
        all_checks.append("PASS: camera+radar overlap detected camera")
    else:
        all_checks.append("FAIL: camera+radar overlap did NOT detect camera")

    if phase2_ok and not phase1_ok:
        all_checks.append("NOTE: overlap passed but camera-only failed — "
                          "unexpected; check road_map.bin fixture")

    ble_drops = (results["metrics"]["parseFailures_delta"]
                 + results["metrics"]["queueDrops_delta"])
    if ble_drops > 0:
        all_checks.append(f"WARNING: BLE drops during test: {ble_drops}")
    else:
        all_checks.append("PASS: zero BLE drops")

    result = "PASS" if phase1_ok and phase2_ok else "FAIL"
    results["result"] = result
    results["checks"] = all_checks

    return results


def write_summary(path: Path, results: dict) -> None:
    lines = [
        "# Camera + Radar Overlap Bench (Real Pipeline)",
        "",
        f"- Result: **{results['result']}**",
        f"- Timestamp (UTC): {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
        "",
        "## Config",
        "",
    ]
    for key, value in results["config"].items():
        lines.append(f"- {key}: `{value}`")

    for phase in results["phases"]:
        lines.extend([
            "",
            f"## Phase: {phase['phase']}",
            "",
            f"- displayActive reached: **{phase['saw_display_active']}**",
            f"- Active at step: {phase['display_active_step']}/{phase['total_steps']}",
            f"- Distance at activation: {phase['display_active_distance']}cm",
            f"- Radar scenario active: {phase['scenario_active']}",
            "",
            "### Checks",
            "",
        ])
        for check in phase["checks"]:
            lines.append(f"- {check}")

    lines.extend([
        "",
        "## Metrics",
        "",
    ])
    for key, value in results["metrics"].items():
        lines.append(f"- {key}: `{value}`")

    lines.extend([
        "",
        "## Verdict",
        "",
    ])
    for check in results["checks"]:
        lines.append(f"- {check}")

    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--metrics-url",
        default=os.environ.get("REAL_FW_METRICS_URL",
                               "http://192.168.160.212/api/debug/metrics"),
    )
    parser.add_argument("--base-url", default="")
    parser.add_argument(
        "--http-timeout-seconds", type=int,
        default=int(os.environ.get("REAL_FW_HTTP_TIMEOUT_SECONDS", "5")),
    )
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--scenario-id", default="RAD-03")
    parser.add_argument("--duration-scale-pct", type=int, default=120)
    parser.add_argument("--speed-mph", type=float, default=40.0)
    parser.add_argument("--trace-steps", type=int, default=20)
    args = parser.parse_args()

    base_url = (args.base_url.rstrip("/") if args.base_url
                else derive_base_url(args.metrics_url))
    out_dir = Path(args.out_dir) if args.out_dir else Path(
        f".artifacts/test_reports/camera_radar_overlap_bench_"
        f"{time.strftime('%Y%m%d_%H%M%S')}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    try:
        results = run_bench(base_url, args)
    except BenchFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    summary_path = out_dir / "summary.md"
    details_path = out_dir / "details.json"
    details_path.write_text(
        json.dumps(results, indent=2, sort_keys=True, default=str),
        encoding="utf-8",
    )
    write_summary(summary_path, results)

    print(f"result: {results['result']}")
    print(f"summary: {summary_path}")
    for check in results.get("checks", []):
        print(f"  {check}")

    return 0 if results["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
