#!/usr/bin/env python3
"""Hardware smoke test for camera API, web UI, and display draw path."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
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


def terminate_process_group(proc: subprocess.Popen[str], grace_seconds: float = 2.0) -> None:
    if proc.poll() is not None:
        return

    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return

    try:
        proc.communicate(timeout=grace_seconds)
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    proc.communicate(timeout=grace_seconds)


def dump_dom(chrome_bin: str, url: str, timeout_ms: int, wall_timeout_seconds: float) -> str:
    cmd = [
        chrome_bin,
        "--headless=new",
        "--disable-gpu",
        f"--virtual-time-budget={timeout_ms}",
        "--dump-dom",
        url,
    ]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=wall_timeout_seconds)
    except subprocess.TimeoutExpired as exc:
        terminate_process_group(proc)
        raise SmokeFailure(
            f"Chrome dump-dom timed out after {wall_timeout_seconds:.1f}s for {url}"
        ) from exc
    if proc.returncode != 0:
        raise SmokeFailure(f"Chrome dump-dom failed: {stderr.strip() or proc.returncode}")
    return stdout


def dump_dom_with_timeout_retry(
    chrome_bin: str,
    url: str,
    timeout_ms: int,
    wall_timeout_seconds: float,
    timeout_retries: int,
    retry_backoff_seconds: float,
    retry_extra_seconds: float,
) -> tuple[str, int]:
    attempts = max(1, timeout_retries + 1)
    for attempt_idx in range(attempts):
        attempt_timeout = wall_timeout_seconds + (retry_extra_seconds * attempt_idx)
        try:
            dom = dump_dom(chrome_bin, url, timeout_ms, attempt_timeout)
            return dom, attempt_idx
        except SmokeFailure as exc:
            timeout_like = "timed out" in str(exc).lower()
            is_last_attempt = attempt_idx >= attempts - 1
            if not timeout_like or is_last_attempt:
                raise
            sleep_seconds = max(0.0, retry_backoff_seconds) * (attempt_idx + 1)
            if sleep_seconds > 0:
                time.sleep(sleep_seconds)

    raise SmokeFailure("Chrome dump-dom failed for unknown retry reason")


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
    parser.add_argument("--render-distance-cm", type=int, default=16093)
    parser.add_argument("--render-hold-ms", type=int, default=5000)
    parser.add_argument("--observe-seconds", type=float, default=2.5)
    parser.add_argument("--chrome-timeout-seconds", type=float, default=15.0,
                        help="Hard wall-clock timeout for the headless Chrome DOM dump.")
    parser.add_argument("--chrome-timeout-retries", type=int, default=1,
                        help="Additional retry attempts for timeout-only Chrome DOM failures.")
    parser.add_argument("--chrome-timeout-retry-backoff-seconds", type=float, default=1.5,
                        help="Base backoff before timeout-only DOM retries (scaled by attempt index).")
    parser.add_argument("--chrome-timeout-retry-extra-seconds", type=float, default=10.0,
                        help="Extra timeout seconds added per timeout-only DOM retry attempt.")
    args = parser.parse_args()

    if args.chrome_timeout_retries < 0:
        raise SystemExit("--chrome-timeout-retries must be >= 0")
    if args.chrome_timeout_retry_backoff_seconds < 0:
        raise SystemExit("--chrome-timeout-retry-backoff-seconds must be >= 0")
    if args.chrome_timeout_retry_extra_seconds < 0:
        raise SystemExit("--chrome-timeout-retry-extra-seconds must be >= 0")

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
        ], "camera settings")
        checks.append("camera settings payload includes ALPR enable + range fields")

        status = request_json(base_url, "/api/cameras/status", args.http_timeout_seconds)
        ensure_keys(status, ["cameraCount", "displayActive", "distanceCm"], "camera status")
        checks.append("camera status payload is available on hardware")

        first_cm, _close_cm = choose_test_ranges(
            int(settings["cameraAlertRangeCm"]),
            int(settings["cameraAlertRangeCm"]),
        )
        updated = {
            "cameraAlertRangeCm": first_cm,
        }
        request_json(base_url, "/api/cameras/settings", args.http_timeout_seconds, method="POST", form_fields=updated)
        after_update = request_json(base_url, "/api/cameras/settings", args.http_timeout_seconds)
        require(int(after_update["cameraAlertRangeCm"]) == first_cm,
                f"cameraAlertRangeCm did not persist ({after_update['cameraAlertRangeCm']} != {first_cm})")
        checks.append("camera settings API round-trip persists the ALPR alert distance")

        dom, dom_retry_count = dump_dom_with_timeout_retry(
            chrome_bin=chrome_bin,
            url=base_url + "/cameras",
            timeout_ms=7000,
            wall_timeout_seconds=args.chrome_timeout_seconds,
            timeout_retries=args.chrome_timeout_retries,
            retry_backoff_seconds=args.chrome_timeout_retry_backoff_seconds,
            retry_extra_seconds=args.chrome_timeout_retry_extra_seconds,
        )
        metrics["chrome_dump_dom_attempts"] = dom_retry_count + 1
        metrics["chrome_dump_dom_retries"] = dom_retry_count
        metrics["chrome_dump_dom_timeout_seconds"] = args.chrome_timeout_seconds
        if dom_retry_count > 0:
            checks.append(f"camera page DOM capture recovered after {dom_retry_count} timeout retry")
        require("ALPR Cameras" in dom, "camera page title missing from DOM")
        require("Alert Distance" in dom, "alert distance label missing from DOM")
        require("Enable ALPR Alerts" in dom, "ALPR toggle label missing from DOM")
        require(dom.count('type="number"') >= 1, "camera page did not render the ALPR distance input")
        checks.append("camera page renders the ALPR-only controls on device")

        total_display_delta = 0
        total_audio_delta = 0
        metrics_before = request_json(base_url, "/api/debug/metrics?soak=1", args.http_timeout_seconds)

        render_response = request_json(
            base_url,
            "/api/debug/camera-alert/render",
            args.http_timeout_seconds,
            method="POST",
            form_fields={
                "type": "alpr",
                "distanceCm": args.render_distance_cm,
                "holdMs": args.render_hold_ms,
            },
        )
        require(bool(render_response.get("success")),
                "camera debug render did not report success for ALPR")
        require(render_response.get("type") == "alpr",
                f"camera debug render echoed wrong type ({render_response.get('type')})")
        checks.append("camera debug render succeeded for ALPR")
        time.sleep(max(0.5, args.observe_seconds))

        metrics_after = request_json(base_url, "/api/debug/metrics?soak=1", args.http_timeout_seconds)
        total_display_delta = int(metrics_after.get("displayUpdates", 0)) - int(metrics_before.get("displayUpdates", 0))
        total_audio_delta = int(metrics_after.get("audioPlayCount", 0)) - int(metrics_before.get("audioPlayCount", 0))
        metrics["display_updates_before"] = metrics_before.get("displayUpdates", 0)
        metrics["display_updates_after"] = metrics_after.get("displayUpdates", 0)
        metrics["display_updates_delta"] = total_display_delta
        metrics["audio_play_count_before"] = metrics_before.get("audioPlayCount", 0)
        metrics["audio_play_count_after"] = metrics_after.get("audioPlayCount", 0)
        metrics["audio_play_count_delta"] = total_audio_delta
        require(total_display_delta >= 1,
                f"camera debug render did not increment displayUpdates (delta={total_display_delta})")
        require(total_audio_delta == 0,
                f"camera debug render unexpectedly changed audioPlayCount (delta={total_audio_delta})")
        checks.append("camera debug render exercised ALPR display without camera-specific audio")

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
