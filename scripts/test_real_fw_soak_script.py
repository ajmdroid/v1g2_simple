#!/usr/bin/env python3
"""Regression tests for real firmware soak result finalization."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parents[1]
FINALIZE_SCRIPT = ROOT / "tools" / "finalize_real_fw_soak_result.py"
WAIT_SCRIPT = ROOT / "tools" / "wait_for_json_endpoint.py"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_json(path: Path, payload: object) -> None:
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def base_manifest() -> dict[str, object]:
    return {
        "schema_version": 1,
        "run_id": "real_fw_soak_test",
        "timestamp_utc": "2026-03-17T00:00:00Z",
        "git_sha": "deadbee",
        "git_ref": "main",
        "run_kind": "real_fw_soak",
        "board_id": "release",
        "env": "waveshare-349",
        "lane": "real-fw-soak",
        "suite_or_profile": "drive_wifi_ap",
        "stress_class": "core",
        "result": "PASS",
        "base_result": "PASS",
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
    }


def run_finalize(
    *,
    runtime_result: str,
    trend_scorer_exit: int,
    scoring_payload: dict[str, object] | None,
    trend_skipped: bool = False,
    allow_inconclusive: bool = False,
    trend_error_detail: str = "",
    trend_summary: str = "- Result: **PASS**\n- Stable.\n",
) -> tuple[dict[str, object], dict[str, object], str]:
    with tempfile.TemporaryDirectory() as tmp_dir:
        root = Path(tmp_dir)
        manifest_path = root / "manifest.json"
        scoring_path = root / "scoring.json"
        summary_body_path = root / "summary_body.md"
        summary_output_path = root / "summary.md"
        trend_summary_path = root / "trend_summary.md"

        write_json(manifest_path, base_manifest())
        if scoring_payload is not None:
            write_json(scoring_path, scoring_payload)
        summary_body_path.write_text("## Runtime Assessment\n\n- Primary bucket: No issues\n", encoding="utf-8")
        trend_summary_path.write_text(trend_summary, encoding="utf-8")

        cmd = [
            sys.executable,
            str(FINALIZE_SCRIPT),
            "--manifest",
            str(manifest_path),
            "--runtime-result",
            runtime_result,
            "--trend-scorer-exit",
            str(trend_scorer_exit),
            "--trend-scoring-json",
            str(scoring_path),
            "--summary-body",
            str(summary_body_path),
            "--summary-output",
            str(summary_output_path),
            "--trend-summary",
            str(trend_summary_path),
        ]
        if trend_skipped:
            cmd.append("--trend-skipped")
        if allow_inconclusive:
            cmd.append("--allow-inconclusive")
        if trend_error_detail:
            cmd.extend(["--trend-error-detail", trend_error_detail])

        completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
        assert_true(completed.returncode == 0, f"finalize helper failed: {completed.stdout}\n{completed.stderr}")
        payload = json.loads(completed.stdout)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        summary = summary_output_path.read_text(encoding="utf-8")
        return payload, manifest, summary


class DelayedJsonHandler(BaseHTTPRequestHandler):
    ready_after_seconds = 0.0

    def do_GET(self) -> None:  # noqa: N802
        elapsed = time.monotonic() - self.server.start_time  # type: ignore[attr-defined]
        if elapsed >= self.ready_after_seconds:
            body = json.dumps({"ok": True}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        body = b"warming"
        self.send_response(503)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:  # noqa: A003
        return


def run_wait_helper(*, ready_after_seconds: float, max_wait_seconds: int, retry_delay_seconds: int) -> tuple[int, dict[str, object]]:
    server = ThreadingHTTPServer(("127.0.0.1", 0), DelayedJsonHandler)
    server.start_time = time.monotonic()  # type: ignore[attr-defined]
    DelayedJsonHandler.ready_after_seconds = ready_after_seconds
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        cmd = [
            sys.executable,
            str(WAIT_SCRIPT),
            "--endpoint-name",
            "metrics endpoint",
            "--url",
            f"http://127.0.0.1:{server.server_port}/api/debug/metrics",
            "--timeout-seconds",
            "1",
            "--retry-delay-seconds",
            str(retry_delay_seconds),
            "--max-wait-seconds",
            str(max_wait_seconds),
        ]
        completed = subprocess.run(cmd, capture_output=True, text=True, check=False)
        payload = json.loads(completed.stdout)
        return completed.returncode, payload
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=1)


def main() -> int:
    warning_payload, warning_manifest, warning_summary = run_finalize(
        runtime_result="PASS",
        trend_scorer_exit=1,
        scoring_payload={"result": "PASS_WITH_WARNINGS"},
        trend_summary="- Result: **PASS_WITH_WARNINGS**\n- Advisory regression.\n",
    )
    assert_true(warning_payload["result"] == "PASS_WITH_WARNINGS", f"unexpected warning result: {warning_payload}")
    assert_true(warning_payload["exit_code"] == 1, f"warning exit code should be failing: {warning_payload}")
    assert_true(warning_manifest["result"] == "PASS_WITH_WARNINGS", f"manifest mismatch: {warning_manifest}")
    assert_true(warning_manifest["runtime_result"] == "PASS", f"manifest runtime missing: {warning_manifest}")
    assert_true(warning_manifest["trend_result"] == "PASS_WITH_WARNINGS", f"manifest trend result missing: {warning_manifest}")
    assert_true("- Result: **PASS_WITH_WARNINGS**" in warning_summary, f"summary missing final result: {warning_summary}")
    assert_true("- Runtime result: **PASS**" in warning_summary, f"summary missing runtime result: {warning_summary}")
    assert_true("- Trend result: **PASS_WITH_WARNINGS**" in warning_summary, f"summary missing trend result: {warning_summary}")

    fail_payload, fail_manifest, fail_summary = run_finalize(
        runtime_result="PASS",
        trend_scorer_exit=2,
        scoring_payload={"result": "FAIL"},
        trend_summary="- Result: **FAIL**\n- Hard regression.\n",
    )
    assert_true(fail_payload["result"] == "FAIL", f"unexpected fail result: {fail_payload}")
    assert_true(fail_payload["exit_code"] == 1, f"fail exit should be 1: {fail_payload}")
    assert_true(fail_manifest["result"] == "FAIL", f"manifest mismatch: {fail_manifest}")
    assert_true("- Result: **FAIL**" in fail_summary, f"summary missing fail result: {fail_summary}")

    skipped_payload, skipped_manifest, skipped_summary = run_finalize(
        runtime_result="PASS",
        trend_scorer_exit=0,
        scoring_payload=None,
        trend_skipped=True,
    )
    assert_true(skipped_payload["result"] == "PASS", f"skipped scoring should preserve runtime result: {skipped_payload}")
    assert_true(skipped_payload["trend_status"] == "skipped", f"unexpected skipped status: {skipped_payload}")
    assert_true("trend_result" not in skipped_manifest, f"skipped run should not set trend_result: {skipped_manifest}")
    assert_true("- Trend status: `skipped`" in skipped_summary, f"summary missing skipped status: {skipped_summary}")
    assert_true("Structured trend scoring skipped because no trend metrics were emitted." in skipped_summary, f"summary missing skip reason: {skipped_summary}")

    error_payload, error_manifest, error_summary = run_finalize(
        runtime_result="PASS",
        trend_scorer_exit=3,
        scoring_payload=None,
        trend_error_detail="Trend scorer crashed.",
    )
    assert_true(error_payload["result"] == "ERROR", f"unexpected error result: {error_payload}")
    assert_true(error_payload["exit_code"] == 1, f"error exit should be 1: {error_payload}")
    assert_true(error_manifest["trend_status"] == "error", f"manifest missing error status: {error_manifest}")
    assert_true(error_manifest["trend_error"] == "Trend scorer crashed.", f"manifest missing trend error: {error_manifest}")
    assert_true("- Result: **ERROR**" in error_summary, f"summary missing error result: {error_summary}")
    assert_true("Trend scorer crashed." in error_summary, f"summary missing trend error detail: {error_summary}")

    fail_on_error_payload, fail_on_error_manifest, fail_on_error_summary = run_finalize(
        runtime_result="FAIL",
        trend_scorer_exit=3,
        scoring_payload=None,
        trend_error_detail="Trend scorer crashed after runtime failure.",
    )
    assert_true(fail_on_error_payload["result"] == "FAIL", f"runtime FAIL should win on scorer error: {fail_on_error_payload}")
    assert_true(fail_on_error_manifest["result"] == "FAIL", f"manifest should keep fail: {fail_on_error_manifest}")
    assert_true("- Result: **FAIL**" in fail_on_error_summary, f"summary missing final fail result: {fail_on_error_summary}")

    inconclusive_payload, _, _ = run_finalize(
        runtime_result="INCONCLUSIVE",
        trend_scorer_exit=0,
        scoring_payload=None,
        trend_skipped=True,
        allow_inconclusive=True,
    )
    assert_true(inconclusive_payload["result"] == "INCONCLUSIVE", f"unexpected inconclusive result: {inconclusive_payload}")
    assert_true(inconclusive_payload["exit_code"] == 0, f"allowed inconclusive should exit zero: {inconclusive_payload}")

    late_success_rc, late_success_payload = run_wait_helper(
        ready_after_seconds=1.2,
        max_wait_seconds=4,
        retry_delay_seconds=1,
    )
    assert_true(late_success_rc == 0, f"late-success wait helper should succeed: {late_success_payload}")
    assert_true(late_success_payload["ok"] is True, f"late-success payload should be ok: {late_success_payload}")
    assert_true(int(late_success_payload["attempts"]) >= 2, f"late-success should take retries: {late_success_payload}")
    assert_true(float(late_success_payload["elapsed_seconds"]) >= 1.0, f"late-success elapsed too short: {late_success_payload}")

    timeout_rc, timeout_payload = run_wait_helper(
        ready_after_seconds=10.0,
        max_wait_seconds=1,
        retry_delay_seconds=0,
    )
    assert_true(timeout_rc == 1, f"timeout wait helper should fail: {timeout_payload}")
    assert_true(timeout_payload["ok"] is False, f"timeout payload should not be ok: {timeout_payload}")
    assert_true("Timed out waiting for metrics endpoint" in str(timeout_payload["reason"]), f"timeout reason missing: {timeout_payload}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
