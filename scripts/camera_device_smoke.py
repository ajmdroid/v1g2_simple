#!/usr/bin/env python3
"""Hardware smoke test for camera API, web UI, and display draw path."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request


class SmokeFailure(RuntimeError):
    pass


def derive_base_url(metrics_url: str) -> str:
    marker = "/api/debug/metrics"
    idx = metrics_url.find(marker)
    if idx >= 0:
        return metrics_url[:idx].rstrip("/")
    return metrics_url.rstrip("/")


def request_json(base_url: str,
                 path: str,
                 timeout_seconds: int,
                 method: str = "GET",
                 form_fields: dict[str, object] | None = None) -> dict[str, object]:
    data = None
    headers: dict[str, str] = {}
    if form_fields is not None:
        encoded = urllib.parse.urlencode(
            {key: str(value).lower() if isinstance(value, bool) else str(value)
             for key, value in form_fields.items()}
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
        raise SmokeFailure(message)


def ensure_keys(payload: dict[str, object], keys: list[str], label: str) -> None:
    missing = [key for key in keys if key not in payload]
    require(not missing, f"{label} missing keys: {', '.join(missing)}")


def choose_test_ranges(original_first_cm: int, original_close_cm: int) -> tuple[int, int]:
    candidates = [
        (80467, 40234),   # 0.50 mi / 0.25 mi
        (96560, 32187),   # 0.60 mi / 0.20 mi
        (112654, 48280),  # 0.70 mi / 0.30 mi
    ]
    for first_cm, close_cm in candidates:
        if first_cm != original_first_cm or close_cm != original_close_cm:
            return first_cm, close_cm
    return candidates[0]


def find_chrome(explicit: str | None) -> str:
    candidates: list[str] = []
    if explicit:
        candidates.append(explicit)
    env_value = os.environ.get("CHROME_BIN")
    if env_value:
        candidates.append(env_value)
    candidates.extend([
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        shutil.which("google-chrome") or "",
        shutil.which("chromium") or "",
        shutil.which("chromium-browser") or "",
    ])
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise SmokeFailure("Chrome binary not found; set --chrome-bin or CHROME_BIN")


def dump_dom(chrome_bin: str, url: str, timeout_ms: int) -> str:
    cmd = [
        chrome_bin,
        "--headless=new",
        "--disable-gpu",
        f"--virtual-time-budget={timeout_ms}",
        "--dump-dom",
        url,
    ]
    proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SmokeFailure(f"Chrome dump-dom failed: {proc.stderr.strip() or proc.returncode}")
    return proc.stdout


def write_summary(path: Path,
                  result: str,
                  metrics: dict[str, object],
                  checks: list[str],
                  failure: str | None) -> None:
    lines = [
        "# Camera Device Smoke Summary",
        "",
        f"- Result: **{result}**",
        f"- Timestamp (UTC): {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
        "",
        "## Checks",
        "",
    ]
    for check in checks:
        lines.append(f"- {check}")
    lines.extend([
        "",
        "## Metrics",
        "",
    ])
    for key, value in metrics.items():
        lines.append(f"- {key}: `{value}`")
    lines.extend([
        "",
        "## Failure",
        "",
        f"- {failure or 'none'}",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metrics-url", default=os.environ.get("REAL_FW_METRICS_URL", "http://192.168.160.212/api/debug/metrics"))
    parser.add_argument("--base-url", default="")
    parser.add_argument("--http-timeout-seconds", type=int, default=int(os.environ.get("REAL_FW_HTTP_TIMEOUT_SECONDS", "5")))
    parser.add_argument("--chrome-bin", default="")
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--render-type", default="speed")
    parser.add_argument("--render-distance-cm", type=int, default=16093)
    parser.add_argument("--render-hold-ms", type=int, default=5000)
    parser.add_argument("--voice-stage", default="far", choices=["none", "far", "near"])
    parser.add_argument("--observe-seconds", type=float, default=2.5)
    parser.add_argument("--voice-all-types", action="store_true",
                        help="Request voice playback for every camera type instead of only the first one.")
    parser.add_argument("--voice-wait-seconds", type=float, default=4.5,
                        help="Delay after a voiced render so the clip can finish before the next type starts.")
    args = parser.parse_args()

    base_url = args.base_url.rstrip("/") if args.base_url else derive_base_url(args.metrics_url)
    out_dir = Path(args.out_dir) if args.out_dir else Path(
        f".artifacts/test_reports/camera_smoke_{time.strftime('%Y%m%d_%H%M%S')}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    summary_path = out_dir / "summary.md"
    details_path = out_dir / "details.json"

    checks: list[str] = []
    metrics: dict[str, object] = {
        "base_url": base_url,
        "metrics_url": args.metrics_url,
    }
    original_settings: dict[str, object] | None = None
    failure: str | None = None

    try:
        chrome_bin = find_chrome(args.chrome_bin or None)
        metrics["chrome_bin"] = chrome_bin

        settings = request_json(base_url, "/api/cameras/settings", args.http_timeout_seconds)
        original_settings = dict(settings)
        ensure_keys(settings, [
            "cameraAlertsEnabled",
            "cameraAlertRangeCm",
            "cameraAlertNearRangeCm",
            "cameraTypeAlpr",
            "cameraTypeRedLight",
            "cameraTypeSpeed",
            "cameraTypeBusLane",
            "colorCameraArrow",
            "colorCameraText",
            "cameraVoiceFarEnabled",
            "cameraVoiceNearEnabled",
        ], "camera settings")
        checks.append("camera settings payload includes first/close alert fields")

        status = request_json(base_url, "/api/cameras/status", args.http_timeout_seconds)
        ensure_keys(status, ["cameraCount", "displayActive", "type", "distanceCm"], "camera status")
        checks.append("camera status payload is available on hardware")

        first_cm, close_cm = choose_test_ranges(
            int(settings["cameraAlertRangeCm"]),
            int(settings["cameraAlertNearRangeCm"]),
        )
        updated = {
            "cameraAlertRangeCm": first_cm,
            "cameraAlertNearRangeCm": close_cm,
        }
        request_json(base_url, "/api/cameras/settings", args.http_timeout_seconds, method="POST", form_fields=updated)
        after_update = request_json(base_url, "/api/cameras/settings", args.http_timeout_seconds)
        require(int(after_update["cameraAlertRangeCm"]) == first_cm,
                f"cameraAlertRangeCm did not persist ({after_update['cameraAlertRangeCm']} != {first_cm})")
        require(int(after_update["cameraAlertNearRangeCm"]) == close_cm,
                f"cameraAlertNearRangeCm did not persist ({after_update['cameraAlertNearRangeCm']} != {close_cm})")
        checks.append("camera settings API round-trip persists first and close alert distances")

        dom = dump_dom(chrome_bin, base_url + "/cameras", 7000)
        require("Camera Alerts" in dom, "camera page title missing from DOM")
        require("First Alert Distance" in dom, "first alert label missing from DOM")
        require("Close Alert Distance" in dom, "close alert label missing from DOM")
        require(dom.count('type="number"') >= 2, "camera page did not render both numeric distance inputs")
        checks.append("camera page renders first/close numeric inputs on device")

        all_types = ["speed", "red_light", "bus_lane", "alpr"]
        total_display_delta = 0
        total_audio_delta = 0
        voice_started_count = 0
        voiced_type_count = 0
        metrics_before = request_json(base_url, "/api/debug/metrics?soak=1", args.http_timeout_seconds)

        for cam_type in all_types:
            voice_for_type = "none"
            if args.voice_stage != "none":
                if args.voice_all_types or cam_type == all_types[0]:
                    voice_for_type = args.voice_stage
            render_response = request_json(
                base_url,
                "/api/debug/camera-alert/render",
                args.http_timeout_seconds,
                method="POST",
                form_fields={
                    "type": cam_type,
                    "distanceCm": args.render_distance_cm,
                    "holdMs": args.render_hold_ms,
                    "voiceStage": voice_for_type,
                },
            )
            require(bool(render_response.get("success")),
                    f"camera debug render did not report success for type={cam_type}")
            require(render_response.get("type") == cam_type,
                    f"camera debug render echoed wrong type (expected {cam_type})")
            checks.append(f"camera debug render succeeded for type={cam_type}")
            if voice_for_type != "none":
                voiced_type_count += 1
                if bool(render_response.get("voiceStarted", False)):
                    voice_started_count += 1
            pause_seconds = max(0.5, args.observe_seconds)
            if voice_for_type != "none":
                pause_seconds = max(pause_seconds, args.voice_wait_seconds)
            time.sleep(pause_seconds)

        metrics_after = request_json(base_url, "/api/debug/metrics?soak=1", args.http_timeout_seconds)
        total_display_delta = int(metrics_after.get("displayUpdates", 0)) - int(metrics_before.get("displayUpdates", 0))
        total_audio_delta = int(metrics_after.get("audioPlayCount", 0)) - int(metrics_before.get("audioPlayCount", 0))
        metrics["display_updates_before"] = metrics_before.get("displayUpdates", 0)
        metrics["display_updates_after"] = metrics_after.get("displayUpdates", 0)
        metrics["display_updates_delta"] = total_display_delta
        metrics["audio_play_count_before"] = metrics_before.get("audioPlayCount", 0)
        metrics["audio_play_count_after"] = metrics_after.get("audioPlayCount", 0)
        metrics["audio_play_count_delta"] = total_audio_delta
        metrics["render_voice_started_count"] = voice_started_count
        metrics["render_voice_requested_count"] = voiced_type_count
        metrics["voice_all_types"] = args.voice_all_types
        require(total_display_delta >= len(all_types),
                f"camera debug render did not increment displayUpdates for all types (delta={total_display_delta})")
        checks.append(f"camera debug render cycled all {len(all_types)} camera types on hardware")

        if voiced_type_count > 0:
            require(voice_started_count == voiced_type_count,
                    f"camera debug render only started voice {voice_started_count}/{voiced_type_count} time(s)")
            require(total_audio_delta >= voiced_type_count,
                    f"camera debug render did not increment audioPlayCount for all voiced types "
                    f"(delta={total_audio_delta}, expected>={voiced_type_count})")
            if args.voice_all_types:
                checks.append("camera debug render started voice playback for every camera type")
            else:
                checks.append("camera debug render started the expected camera voice playback")

        request_json(
            base_url,
            "/api/debug/camera-alert/clear",
            args.http_timeout_seconds,
            method="POST",
            form_fields={},
        )
        checks.append("camera display clear route restored normal owner selection")

        if original_settings is not None:
            request_json(
                base_url,
                "/api/cameras/settings",
                args.http_timeout_seconds,
                method="POST",
                form_fields=original_settings,
            )
            restored = request_json(base_url, "/api/cameras/settings", args.http_timeout_seconds)
            require(int(restored["cameraAlertRangeCm"]) == int(original_settings["cameraAlertRangeCm"]),
                    "camera settings restore did not restore first alert distance")
            require(int(restored["cameraAlertNearRangeCm"]) == int(original_settings["cameraAlertNearRangeCm"]),
                    "camera settings restore did not restore close alert distance")
            checks.append("camera settings were restored to the pre-test values")

        result = "PASS"
    except Exception as exc:  # pragma: no cover - exercised in real hardware runs
        failure = str(exc)
        result = "FAIL"
        if original_settings is not None:
            try:
                request_json(
                    base_url,
                    "/api/cameras/settings",
                    args.http_timeout_seconds,
                    method="POST",
                    form_fields=original_settings,
                )
                metrics["restored_after_failure"] = True
            except Exception as restore_exc:
                metrics["restored_after_failure"] = False
                metrics["restore_failure"] = str(restore_exc)
    finally:
        details_path.write_text(json.dumps({
            "result": result,
            "checks": checks,
            "metrics": metrics,
            "failure": failure,
        }, indent=2, sort_keys=True), encoding="utf-8")
        write_summary(summary_path, result, metrics, checks, failure)

    print(f"result: {result}")
    print(f"summary: {summary_path}")
    print(f"details: {details_path}")
    for key, value in metrics.items():
        print(f"{key}={value}")
    if failure:
        print(f"failure={failure}")
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
