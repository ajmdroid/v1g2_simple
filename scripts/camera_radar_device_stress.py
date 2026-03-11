#!/usr/bin/env python3
"""Focused hardware stress runner for overlapping camera debug render with a V1 scenario."""

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


class StressFailure(RuntimeError):
    pass


REQUIRED_CAMERA_METRIC_KEYS = (
    "cameraDisplayActive",
    "cameraDebugOverrideActive",
    "cameraDisplayFrames",
    "cameraDebugDisplayFrames",
    "cameraDisplayMaxUs",
    "cameraDebugDisplayMaxUs",
    "cameraProcessMaxUs",
)


def derive_base_url(metrics_url: str) -> str:
    marker = "/api/debug/metrics"
    idx = metrics_url.find(marker)
    if idx >= 0:
        return metrics_url[:idx].rstrip("/")
    return metrics_url.rstrip("/")


def request_json(
    base_url: str,
    path: str,
    timeout_seconds: int,
    method: str = "GET",
    json_body: dict[str, object] | None = None,
    form_fields: dict[str, object] | None = None,
) -> dict[str, object]:
    data = None
    headers: dict[str, str] = {}
    if json_body is not None:
        data = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    elif form_fields is not None:
        encoded = urllib.parse.urlencode(
            {
                key: str(value).lower() if isinstance(value, bool) else str(value)
                for key, value in form_fields.items()
            }
        )
        data = encoded.encode("utf-8")
        headers["Content-Type"] = "application/x-www-form-urlencoded"
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
        raise StressFailure(message)


def require_camera_metric_keys(metrics: dict[str, object], context: str) -> None:
    missing = [key for key in REQUIRED_CAMERA_METRIC_KEYS if key not in metrics]
    require(not missing, f"{context}: metrics payload missing keys: {', '.join(missing)}")


def resolve_scenario_timeout_seconds(scale_pct: int) -> int:
    auto_timeout = max(25, math.ceil((25 * scale_pct) / 100.0))
    return max(60, auto_timeout + 30)


def metric_int(metrics: dict[str, object], key: str) -> int:
    return int(metrics.get(key, 0) or 0)


def metric_delta(before: dict[str, object], after: dict[str, object], key: str) -> int:
    return metric_int(after, key) - metric_int(before, key)


def classify_camera_activity(
    before: dict[str, object], after: dict[str, object]
) -> tuple[bool, bool, str]:
    real_active = (
        metric_int(after, "cameraDisplayActive") > 0
        or metric_delta(before, after, "cameraDisplayFrames") > 0
    )
    debug_active = (
        metric_int(after, "cameraDebugOverrideActive") > 0
        or metric_delta(before, after, "cameraDebugDisplayFrames") > 0
    )
    if real_active and debug_active:
        return real_active, debug_active, "both"
    if real_active:
        return real_active, debug_active, "real"
    if debug_active:
        return real_active, debug_active, "debug"
    return real_active, debug_active, "neither"


def analyze_samples(
    pre_metrics: dict[str, object], samples: list[dict[str, object]], disp_pipe_limit_us: int
) -> tuple[dict[str, object], list[dict[str, object]], list[str]]:
    if not samples:
        return (
            {
                "sample_count": 0,
                "duration_s": 0.0,
                "camera_correlated_disp_pipe_peak_us": 0,
                "camera_correlated_disp_pipe_sample_count": 0,
                "over_limit_real_samples": 0,
                "over_limit_debug_samples": 0,
                "over_limit_both_samples": 0,
                "over_limit_neither_samples": 0,
            },
            [],
            [],
        )

    last_metrics = samples[-1]["metrics"]
    delta_keys = [
        "rxPackets",
        "parseSuccesses",
        "parseFailures",
        "queueDrops",
        "oversizeDrops",
        "bleMutexTimeout",
        "displayUpdates",
        "audioPlayCount",
        "audioPlayBusy",
        "cameraDisplayFrames",
        "cameraDebugDisplayFrames",
    ]
    metrics = {
        f"{key}_delta": metric_delta(pre_metrics, last_metrics, key)
        for key in delta_keys
    }
    metrics["camera_display_frame_delta"] = metrics.pop("cameraDisplayFrames_delta")
    metrics["camera_debug_frame_delta"] = metrics.pop("cameraDebugDisplayFrames_delta")

    metrics.update(
        {
            "sample_count": len(samples),
            "duration_s": round(
                max(0.0, float(samples[-1]["sample_elapsed_s"]) - float(samples[0]["sample_elapsed_s"])),
                3,
            ),
            "dispPipeMaxUs_peak": max(metric_int(sample["metrics"], "dispPipeMaxUs") for sample in samples),
            "loopMaxUs_peak": max(metric_int(sample["metrics"], "loopMaxUs") for sample in samples),
            "wifiMaxUs_peak": max(metric_int(sample["metrics"], "wifiMaxUs") for sample in samples),
            "flushMaxUs_peak": max(metric_int(sample["metrics"], "flushMaxUs") for sample in samples),
            "cameraDisplayMaxUs_peak": max(
                metric_int(sample["metrics"], "cameraDisplayMaxUs") for sample in samples
            ),
            "cameraDebugDisplayMaxUs_peak": max(
                metric_int(sample["metrics"], "cameraDebugDisplayMaxUs") for sample in samples
            ),
            "cameraProcessMaxUs_peak": max(
                metric_int(sample["metrics"], "cameraProcessMaxUs") for sample in samples
            ),
        }
    )

    breakdown = {
        "real": 0,
        "debug": 0,
        "both": 0,
        "neither": 0,
    }
    over_limit_samples: list[dict[str, object]] = []
    prev_metrics = pre_metrics
    camera_correlated_peak_us = 0
    camera_correlated_sample_count = 0
    for sample in samples:
        sample_metrics = sample["metrics"]
        disp_pipe_us = metric_int(sample_metrics, "dispPipeMaxUs")
        if disp_pipe_us > disp_pipe_limit_us:
            real_active, debug_active, activity_kind = classify_camera_activity(prev_metrics, sample_metrics)
            breakdown[activity_kind] += 1
            if real_active or debug_active:
                camera_correlated_sample_count += 1
                camera_correlated_peak_us = max(camera_correlated_peak_us, disp_pipe_us)
            over_limit_samples.append(
                {
                    "sample_elapsed_s": sample["sample_elapsed_s"],
                    "dispPipeMaxUs": disp_pipe_us,
                    "loopMaxUs": metric_int(sample_metrics, "loopMaxUs"),
                    "flushMaxUs": metric_int(sample_metrics, "flushMaxUs"),
                    "wifiMaxUs": metric_int(sample_metrics, "wifiMaxUs"),
                    "cameraActivity": activity_kind,
                    "cameraDisplayFramesDelta": metric_delta(prev_metrics, sample_metrics, "cameraDisplayFrames"),
                    "cameraDebugDisplayFramesDelta": metric_delta(
                        prev_metrics, sample_metrics, "cameraDebugDisplayFrames"
                    ),
                }
            )
        prev_metrics = sample_metrics

    metrics.update(
        {
            "camera_correlated_disp_pipe_peak_us": camera_correlated_peak_us,
            "camera_correlated_disp_pipe_sample_count": camera_correlated_sample_count,
            "over_limit_real_samples": breakdown["real"],
            "over_limit_debug_samples": breakdown["debug"],
            "over_limit_both_samples": breakdown["both"],
            "over_limit_neither_samples": breakdown["neither"],
        }
    )

    failure_reasons: list[str] = []
    for key in ("parseFailures", "queueDrops", "oversizeDrops", "bleMutexTimeout"):
        delta_value = metric_delta(pre_metrics, last_metrics, key)
        if delta_value > 0:
            failure_reasons.append(f"{key} delta {delta_value} > 0")
    if camera_correlated_sample_count > 0:
        failure_reasons.append(
            "camera-correlated dispPipeMaxUs over limit "
            f"({camera_correlated_sample_count} samples, peak {camera_correlated_peak_us} us > {disp_pipe_limit_us})"
        )

    return metrics, over_limit_samples, failure_reasons


def write_summary(
    path: Path,
    result: str,
    config: dict[str, object],
    phase_results: list[dict[str, object]],
) -> None:
    lines = [
        "# Camera + Radar Stress Summary",
        "",
        f"- Result: **{result}**",
        f"- Timestamp (UTC): {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
        "",
        "## Config",
        "",
    ]
    for key, value in config.items():
        lines.append(f"- {key}: `{value}`")
    for phase in phase_results:
        lines.extend(
            [
                "",
                f"## {phase['phase_label']}",
                "",
                f"- Result: **{phase['result']}**",
                f"- Failure: `{phase['failure'] or 'none'}`",
                f"- Samples: `{phase['sample_count']}`",
                f"- Samples path: `{phase['samples_path']}`",
                "",
                "### Checks",
                "",
            ]
        )
        for check in phase["checks"]:
            lines.append(f"- {check}")
        lines.extend(
            [
                "",
                "### Metrics",
                "",
            ]
        )
        for key, value in phase["metrics"].items():
            lines.append(f"- {key}: `{value}`")
        if phase.get("actions"):
            lines.extend(
                [
                    "",
                    "### Actions",
                    "",
                ]
            )
            for action in phase["actions"]:
                lines.append(
                    f"- {action['elapsed_s']}s `{action['action']}` hold_ms=`{action.get('hold_ms', 0)}`"
                )
        if phase.get("over_limit_samples"):
            lines.extend(
                [
                    "",
                    "### Over-Limit Samples",
                    "",
                ]
            )
            for sample in phase["over_limit_samples"]:
                lines.append(
                    "- "
                    f"{sample['sample_elapsed_s']}s "
                    f"dispPipe={sample['dispPipeMaxUs']} "
                    f"activity={sample['cameraActivity']} "
                    f"loop={sample['loopMaxUs']} flush={sample['flushMaxUs']} wifi={sample['wifiMaxUs']}"
                )
    path.write_text("\n".join(lines), encoding="utf-8")


def run_phase(
    phase_name: str,
    base_url: str,
    args: argparse.Namespace,
    scenario_timeout_seconds: int,
    disp_pipe_limit_us: int,
    out_dir: Path,
) -> dict[str, object]:
    phase_label = "Steady-State" if phase_name == "overlap" else "Transition"
    samples_path = out_dir / f"{phase_name}_samples.jsonl"
    if samples_path.exists():
        samples_path.unlink()

    checks: list[str] = []
    actions: list[dict[str, object]] = []
    failure: str | None = None
    result = "FAIL"
    samples: list[dict[str, object]] = []
    pre_metrics: dict[str, object] = {}
    render_response: dict[str, object] = {}
    start_response: dict[str, object] = {}
    final_status: dict[str, object] = {}
    analyzed_metrics: dict[str, object] = {}
    over_limit_samples: list[dict[str, object]] = []
    effective_flap_cycles = args.flap_cycles
    cleanup_camera_clear = False
    cleanup_scenario_stop = False

    try:
        request_json(base_url, "/api/debug/camera-alert/clear", args.http_timeout_seconds, method="POST", form_fields={})
        request_json(base_url, "/api/debug/v1-scenario/stop", args.http_timeout_seconds, method="POST", json_body={})

        reset_response = request_json(
            base_url, "/api/debug/metrics/reset", args.http_timeout_seconds, method="POST", json_body={}
        )
        require(bool(reset_response.get("success")), f"{phase_name}: metrics reset failed")
        checks.append("metrics reset succeeded")

        pre_metrics = request_json(base_url, "/api/debug/metrics?soak=1", args.http_timeout_seconds)
        require_camera_metric_keys(pre_metrics, f"{phase_name}: pre-run")
        checks.append("pre-run metrics snapshot succeeded")

        start_response = request_json(
            base_url,
            "/api/debug/v1-scenario/start",
            args.http_timeout_seconds,
            method="POST",
            json_body={
                "id": args.scenario_id,
                "loop": False,
                "streamRepeatMs": 700,
                "durationScalePct": args.duration_scale_pct,
            },
        )
        require(bool(start_response.get("success")), f"{phase_name}: scenario start failed")
        checks.append("scenario start succeeded")

        if phase_name == "flap":
            interval_seconds = max(0.5, args.flap_interval_seconds)
            scenario_duration_seconds = float(start_response.get("durationMs", 0) or 0) / 1000.0
            max_cycles_by_duration = int((scenario_duration_seconds / interval_seconds + 1.0) // 2.0)
            max_cycles_by_duration = max(1, max_cycles_by_duration)
            if effective_flap_cycles > max_cycles_by_duration:
                checks.append(
                    "flap cycles capped to "
                    f"{max_cycles_by_duration}/{effective_flap_cycles} "
                    f"for duration {scenario_duration_seconds:.3f}s at interval {interval_seconds:.3f}s"
                )
                effective_flap_cycles = max_cycles_by_duration

        t0 = time.time()
        next_sample_at = t0

        if phase_name == "overlap":
            effective_hold_ms = max(args.hold_ms, (scenario_timeout_seconds + 5) * 1000)
            render_response = request_json(
                base_url,
                "/api/debug/camera-alert/render",
                args.http_timeout_seconds,
                method="POST",
                form_fields={
                    "type": args.camera_type,
                    "distanceCm": args.camera_distance_cm,
                    "holdMs": effective_hold_ms,
                },
            )
            require(bool(render_response.get("success")), "overlap: camera debug render failed")
            actions.append(
                {
                    "elapsed_s": 0.0,
                    "action": "render",
                    "hold_ms": int(render_response.get("holdMs", effective_hold_ms) or effective_hold_ms),
                }
            )
            checks.append("camera debug render succeeded")
        elif phase_name == "flap":
            checks.append("flap transition driver armed")
        else:
            raise StressFailure(f"unsupported phase '{phase_name}'")

        flap_camera_active = False
        flap_remaining_cycles = effective_flap_cycles
        flap_next_action_at = t0

        while True:
            now = time.time()
            elapsed_s = now - t0
            if elapsed_s > scenario_timeout_seconds:
                raise StressFailure(
                    f"{phase_name}: scenario timed out waiting for completion (> {scenario_timeout_seconds}s)"
                )

            if phase_name == "flap" and flap_remaining_cycles > 0 and now >= flap_next_action_at:
                if not flap_camera_active:
                    render_response = request_json(
                        base_url,
                        "/api/debug/camera-alert/render",
                        args.http_timeout_seconds,
                        method="POST",
                        form_fields={
                            "type": args.camera_type,
                            "distanceCm": args.camera_distance_cm,
                            "holdMs": args.hold_ms,
                        },
                    )
                    require(bool(render_response.get("success")), "flap: camera debug render failed")
                    flap_camera_active = True
                    actions.append(
                        {
                            "elapsed_s": round(elapsed_s, 3),
                            "action": "render",
                            "hold_ms": int(render_response.get("holdMs", args.hold_ms) or args.hold_ms),
                        }
                    )
                else:
                    clear_resp = request_json(
                        base_url,
                        "/api/debug/camera-alert/clear",
                        args.http_timeout_seconds,
                        method="POST",
                        form_fields={},
                    )
                    require(bool(clear_resp.get("success")), "flap: camera debug clear failed")
                    flap_camera_active = False
                    flap_remaining_cycles -= 1
                    actions.append({"elapsed_s": round(elapsed_s, 3), "action": "clear", "hold_ms": 0})
                flap_next_action_at = now + max(0.5, args.flap_interval_seconds)

            if now >= next_sample_at:
                metrics = request_json(base_url, "/api/debug/metrics?soak=1", args.http_timeout_seconds)
                require_camera_metric_keys(metrics, f"{phase_name}: sample")
                status = request_json(base_url, "/api/debug/v1-scenario/status", args.http_timeout_seconds)
                sample = {
                    "sample_elapsed_s": round(elapsed_s, 3),
                    "metrics": metrics,
                    "status": status,
                }
                samples.append(sample)
                with samples_path.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(sample, sort_keys=True) + "\n")
                final_status = status
                next_sample_at = now + max(0.25, args.sample_seconds)
                if not bool(status.get("running", False)):
                    break
            time.sleep(0.1)

        require(samples, f"{phase_name}: no samples collected")
        require(int(final_status.get("eventsTotal", 0) or 0) > 0, f"{phase_name}: scenario eventsTotal is 0")
        require(
            int(final_status.get("eventsEmitted", 0) or 0) >= int(final_status.get("eventsTotal", 0) or 0),
            f"{phase_name}: scenario eventsEmitted < eventsTotal",
        )
        require(int(final_status.get("completedRuns", 0) or 0) >= 1, f"{phase_name}: scenario completedRuns < 1")
        if phase_name == "flap":
            completed_flap_cycles = effective_flap_cycles - flap_remaining_cycles
            require(
                completed_flap_cycles >= effective_flap_cycles,
                f"flap: completed {completed_flap_cycles}/{effective_flap_cycles} effective flap cycle(s)",
            )
            checks.append(
                f"flap transition cycles completed ({completed_flap_cycles}/{effective_flap_cycles})"
            )
        checks.append("scenario completed and emitted all events")
        analyzed_metrics, over_limit_samples, failure_reasons = analyze_samples(
            pre_metrics, samples, disp_pipe_limit_us
        )
        if failure_reasons:
            raise StressFailure("; ".join(failure_reasons))
        checks.append("parser and queue integrity gates passed")
        if over_limit_samples:
            checks.append("dispPipe over-limit samples were non-camera-correlated only")
        else:
            checks.append("no dispPipe over-limit samples observed")
        result = "PASS"
    except Exception as exc:
        failure = str(exc)
    finally:
        try:
            clear_resp = request_json(
                base_url,
                "/api/debug/camera-alert/clear",
                args.http_timeout_seconds,
                method="POST",
                form_fields={},
            )
            cleanup_camera_clear = bool(clear_resp.get("success"))
        except Exception:
            cleanup_camera_clear = False
        try:
            stop_resp = request_json(
                base_url,
                "/api/debug/v1-scenario/stop",
                args.http_timeout_seconds,
                method="POST",
                json_body={},
            )
            cleanup_scenario_stop = bool(stop_resp.get("success"))
        except Exception:
            cleanup_scenario_stop = False

        if samples and not analyzed_metrics:
            analyzed_metrics, over_limit_samples, _ = analyze_samples(
                pre_metrics, samples, disp_pipe_limit_us
            )

    metrics = {
        "scenario_events_total": final_status.get("eventsTotal", 0),
        "scenario_events_emitted": final_status.get("eventsEmitted", 0),
        "scenario_completed_runs": final_status.get("completedRuns", 0),
        "cleanup_camera_clear": cleanup_camera_clear,
        "cleanup_scenario_stop": cleanup_scenario_stop,
        "pre_rx_packets": pre_metrics.get("rxPackets", 0),
        "requested_flap_cycles": args.flap_cycles if phase_name == "flap" else 0,
        "effective_flap_cycles": effective_flap_cycles if phase_name == "flap" else 0,
        "completed_flap_cycles": (effective_flap_cycles - flap_remaining_cycles) if phase_name == "flap" else 0,
        **analyzed_metrics,
    }
    return {
        "phase": phase_name,
        "phase_label": phase_label,
        "result": result,
        "checks": checks,
        "metrics": metrics,
        "failure": failure,
        "render_response": render_response,
        "start_response": start_response,
        "final_status": final_status,
        "sample_count": len(samples),
        "samples_path": str(samples_path),
        "over_limit_samples": over_limit_samples,
        "actions": actions,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--metrics-url",
        default=os.environ.get("REAL_FW_METRICS_URL", "http://192.168.160.212/api/debug/metrics"),
    )
    parser.add_argument("--base-url", default="")
    parser.add_argument(
        "--http-timeout-seconds",
        type=int,
        default=int(os.environ.get("REAL_FW_HTTP_TIMEOUT_SECONDS", "5")),
    )
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--scenario-id", default="RAD-03")
    parser.add_argument("--duration-scale-pct", type=int, default=120)
    parser.add_argument("--camera-type", default="alpr")
    parser.add_argument("--camera-distance-cm", type=int, default=16093)
    parser.add_argument("--hold-ms", type=int, default=45000)
    parser.add_argument("--mode", default="both", choices=["overlap", "flap", "both"])
    parser.add_argument("--flap-cycles", type=int, default=3)
    parser.add_argument("--flap-interval-seconds", type=float, default=5.0)
    parser.add_argument("--sample-seconds", type=float, default=2.0)
    parser.add_argument("--scenario-timeout-seconds", type=int, default=0)
    args = parser.parse_args()

    base_url = args.base_url.rstrip("/") if args.base_url else derive_base_url(args.metrics_url)
    out_dir = Path(args.out_dir) if args.out_dir else Path(
        f".artifacts/test_reports/camera_radar_stress_{time.strftime('%Y%m%d_%H%M%S')}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    summary_path = out_dir / "summary.md"
    details_path = out_dir / "details.json"

    config = {
        "base_url": base_url,
        "metrics_url": args.metrics_url,
        "scenario_id": args.scenario_id,
        "duration_scale_pct": args.duration_scale_pct,
        "camera_type": args.camera_type,
        "camera_distance_cm": args.camera_distance_cm,
        "hold_ms": args.hold_ms,
        "mode": args.mode,
        "flap_cycles": args.flap_cycles,
        "flap_interval_seconds": args.flap_interval_seconds,
        "sample_seconds": args.sample_seconds,
    }
    disp_pipe_limit_us = 80000
    config["disp_pipe_limit_us"] = disp_pipe_limit_us

    scenario_timeout_seconds = (
        args.scenario_timeout_seconds
        if args.scenario_timeout_seconds > 0
        else resolve_scenario_timeout_seconds(args.duration_scale_pct)
    )
    config["scenario_timeout_seconds"] = scenario_timeout_seconds
    phase_order = ["overlap", "flap"] if args.mode == "both" else [args.mode]
    phase_results = [
        run_phase(
            phase_name=phase_name,
            base_url=base_url,
            args=args,
            scenario_timeout_seconds=scenario_timeout_seconds,
            disp_pipe_limit_us=disp_pipe_limit_us,
            out_dir=out_dir,
        )
        for phase_name in phase_order
    ]
    result = "PASS" if all(phase["result"] == "PASS" for phase in phase_results) else "FAIL"
    details_path.write_text(
        json.dumps(
            {
                "result": result,
                "config": config,
                "phases": phase_results,
            },
            indent=2,
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    write_summary(summary_path, result, config, phase_results)
    print(f"result: {result}")
    print(f"summary: {summary_path}")
    for phase in phase_results:
        if phase["failure"]:
            print(f"{phase['phase']}_failure={phase['failure']}")
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
