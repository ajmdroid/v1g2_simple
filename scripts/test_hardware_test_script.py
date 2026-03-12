#!/usr/bin/env python3
"""Regression test for the single-entrypoint hardware test script."""

from __future__ import annotations

import csv
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TEST_SCRIPT = ROOT / "scripts" / "hardware" / "test.sh"


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def create_fake_child_script(path: Path, child_type: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"""#!/usr/bin/env bash
set -euo pipefail

OUT_DIR=""
COMPARE_TO=""
STEP_KIND="{child_type}"
BOARD_ID="${{DEVICE_BOARD_ID:-release}}"
if [[ "$STEP_KIND" == "soak" ]]; then
  STEP_KIND="core"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir)
      OUT_DIR="$2"
      shift
      ;;
    --compare-to)
      COMPARE_TO="$2"
      shift
      ;;
    --drive-display-preview)
      STEP_KIND="display"
      ;;
  esac
  shift
done

mkdir -p "$OUT_DIR"
if [[ -n "$COMPARE_TO" ]]; then
  printf "%s\\n" "$COMPARE_TO" > "$OUT_DIR/received_compare_to.txt"
fi

STEP_UPPER="$(printf "%s" "$STEP_KIND" | tr '[:lower:]' '[:upper:]')"
RESULT_VAR="FAKE_${{STEP_UPPER}}_RESULT"
COMPARE_VAR="FAKE_${{STEP_UPPER}}_COMPARE_KIND"
VALUE_VAR="FAKE_${{STEP_UPPER}}_VALUE"
BASELINE_VAR="FAKE_${{STEP_UPPER}}_BASELINE"

python3 - "$OUT_DIR" "$COMPARE_TO" "$STEP_KIND" "$BOARD_ID" "${{!RESULT_VAR}}" "${{!COMPARE_VAR}}" "${{!VALUE_VAR}}" "${{!BASELINE_VAR:-}}" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

out_dir = Path(sys.argv[1])
compare_to = sys.argv[2]
step_kind = sys.argv[3]
board_id = sys.argv[4]
result = sys.argv[5]
compare_kind = sys.argv[6]
value = float(sys.argv[7])
baseline_raw = sys.argv[8]
baseline_value = None if baseline_raw == "" else float(baseline_raw)

suite_or_profile = {{
    "device": "device_suite_collection",
    "core": "drive_wifi_ap",
    "display": "drive_wifi_ap_display",
}}[step_kind]
run_kind = "device_suite" if step_kind == "device" else "real_fw_soak"
lane = "device-tests" if step_kind == "device" else "real-fw-soak"
stress_class = "core"
run_id = f"{{step_kind}}_{{datetime.now(timezone.utc).strftime('%Y%m%d%H%M%S')}}"

(out_dir / "metrics.ndjson").write_text(
    json.dumps({{
        "schema_version": 1,
        "run_id": run_id,
        "git_sha": "fakegit",
        "run_kind": run_kind,
        "suite_or_profile": suite_or_profile,
        "metric": f"{{step_kind}}_metric",
        "sample": "value",
        "value": value,
        "unit": "count",
        "tags": {{}},
    }}) + "\\n",
    encoding="utf-8",
)

manifest = {{
    "schema_version": 1,
    "run_id": run_id,
    "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    "git_sha": "fakegit",
    "git_ref": "main",
    "run_kind": run_kind,
    "board_id": board_id,
    "env": "device" if step_kind == "device" else "waveshare-349",
    "lane": lane,
    "suite_or_profile": suite_or_profile,
    "stress_class": stress_class,
    "result": result,
    "base_result": "PASS",
    "metrics_file": "metrics.ndjson",
    "scoring_file": "scoring.json",
    "tracks": [suite_or_profile],
}}
(out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\\n", encoding="utf-8")

baseline_manifest = None
if compare_to:
    baseline_manifest = {{
        "path": compare_to,
        "run_id": "baseline",
        "git_sha": "baselinegit",
        "git_ref": "main",
    }}

scoring = {{
    "schema_version": 1,
    "manifest": {{
        "path": str(out_dir / "manifest.json"),
        "run_id": run_id,
        "git_sha": "fakegit",
        "git_ref": "main",
        "run_kind": run_kind,
        "board_id": board_id,
        "env": "device" if step_kind == "device" else "waveshare-349",
        "lane": lane,
        "suite_or_profile": suite_or_profile,
        "stress_class": stress_class,
        "base_result": "PASS",
    }},
    "baseline_manifest": baseline_manifest,
    "comparison_kind": compare_kind,
    "result": result,
    "summary": {{
        "metrics_scored": 1,
        "hard_failures": 0,
        "advisory_failures": 0,
        "info_regressions": 0,
        "missing_required": 0,
        "missing_optional": 0,
    }},
    "metrics": [
        {{
            "metric": f"{{step_kind}}_metric",
            "run_kind": run_kind,
            "suite_or_profile": suite_or_profile,
            "unit": "count",
            "score_level": "info",
            "required": True,
            "current_value": value,
            "baseline_value": baseline_value,
            "delta_abs": None if baseline_value is None else value - baseline_value,
            "delta_pct": None,
            "sample_count": 1,
            "classification": "no_baseline" if baseline_value is None else "changed",
            "absolute_state": "pass",
            "regression_state": "n/a" if baseline_value is None else "pass",
            "score_status": "pass",
            "messages": [],
        }}
    ],
}}
(out_dir / "scoring.json").write_text(json.dumps(scoring, indent=2) + "\\n", encoding="utf-8")
PY
""",
        encoding="utf-8",
    )
    path.chmod(0o755)


def write_metrics_jsonl(path: Path, *, include_display_counters: bool = True) -> None:
    start = datetime(2026, 3, 12, 12, 0, 0, tzinfo=timezone.utc)
    records = []
    samples = [
        {
            "heapFree": 210000,
            "heapMinFree": 190000,
            "heapDma": 30000,
            "heapDmaLargest": 20000,
            "latencyMaxUs": 50000,
            "flushMaxUs": 20000,
            "loopMaxUs": 150000,
            "wifiMaxUs": 1200,
            "bleDrainMaxUs": 3500,
            "dispPipeMaxUs": 18000,
            "uptimeMs": 1000,
            "displayUpdates": 0,
            "displaySkips": 0,
            "rxPackets": 0,
            "parseSuccesses": 0,
            "parseFailures": 0,
            "queueDrops": 0,
            "perfDrop": 0,
            "wifiApUpTransitions": 1,
            "wifiApDownTransitions": 0,
            "proxyAdvertisingOnTransitions": 1,
            "proxyAdvertisingOffTransitions": 0,
            "wifiApActive": 1,
            "proxyAdvertising": 1,
            "wifiApLastTransitionReasonCode": 0,
            "wifiApLastTransitionReason": "boot",
            "proxyAdvertisingLastTransitionReasonCode": 0,
            "proxyAdvertisingLastTransitionReason": "boot",
            "oversizeDrops": 0,
            "sdMaxUs": 8000,
            "fsMaxUs": 8000,
            "queueHighWater": 2,
            "wifiConnectDeferred": 0,
            "reconnects": 0,
            "disconnects": 0,
            "heapDmaMin": 26000,
            "heapDmaLargestMin": 15000,
            "bleProcessMaxUs": 30000,
            "bleMutexTimeout": 0,
            "gpsObsDrops": 0,
            "proxy": {"dropCount": 0, "advertising": 1},
            "eventBus": {"publishCount": 0, "dropCount": 0, "size": 64},
            "lockout": {"coreGuardTripped": False},
        },
        {
            "heapFree": 205000,
            "heapMinFree": 185000,
            "heapDma": 28000,
            "heapDmaLargest": 18000,
            "latencyMaxUs": 60000,
            "flushMaxUs": 25000,
            "loopMaxUs": 170000,
            "wifiMaxUs": 1600,
            "bleDrainMaxUs": 4200,
            "dispPipeMaxUs": 20000,
            "uptimeMs": 2000,
            "displayUpdates": 1,
            "displaySkips": 0,
            "rxPackets": 60,
            "parseSuccesses": 60,
            "parseFailures": 0,
            "queueDrops": 0,
            "perfDrop": 0,
            "wifiApUpTransitions": 1,
            "wifiApDownTransitions": 0,
            "proxyAdvertisingOnTransitions": 1,
            "proxyAdvertisingOffTransitions": 0,
            "wifiApActive": 1,
            "proxyAdvertising": 1,
            "wifiApLastTransitionReasonCode": 0,
            "wifiApLastTransitionReason": "steady",
            "proxyAdvertisingLastTransitionReasonCode": 0,
            "proxyAdvertisingLastTransitionReason": "steady",
            "oversizeDrops": 0,
            "sdMaxUs": 9000,
            "fsMaxUs": 9000,
            "queueHighWater": 3,
            "wifiConnectDeferred": 1,
            "reconnects": 0,
            "disconnects": 0,
            "heapDmaMin": 25500,
            "heapDmaLargestMin": 14500,
            "bleProcessMaxUs": 35000,
            "bleMutexTimeout": 0,
            "gpsObsDrops": 0,
            "proxy": {"dropCount": 0, "advertising": 1},
            "eventBus": {"publishCount": 10, "dropCount": 0, "size": 64},
            "lockout": {"coreGuardTripped": False},
        },
        {
            "heapFree": 200000,
            "heapMinFree": 180000,
            "heapDma": 26000,
            "heapDmaLargest": 16000,
            "latencyMaxUs": 70000,
            "flushMaxUs": 30000,
            "loopMaxUs": 200000,
            "wifiMaxUs": 1800,
            "bleDrainMaxUs": 5000,
            "dispPipeMaxUs": 21000,
            "uptimeMs": 3000,
            "displayUpdates": 3,
            "displaySkips": 0,
            "rxPackets": 120,
            "parseSuccesses": 120,
            "parseFailures": 0,
            "queueDrops": 0,
            "perfDrop": 0,
            "wifiApUpTransitions": 1,
            "wifiApDownTransitions": 0,
            "proxyAdvertisingOnTransitions": 1,
            "proxyAdvertisingOffTransitions": 0,
            "wifiApActive": 1,
            "proxyAdvertising": 1,
            "wifiApLastTransitionReasonCode": 0,
            "wifiApLastTransitionReason": "steady",
            "proxyAdvertisingLastTransitionReasonCode": 0,
            "proxyAdvertisingLastTransitionReason": "steady",
            "oversizeDrops": 0,
            "sdMaxUs": 10000,
            "fsMaxUs": 10000,
            "queueHighWater": 4,
            "wifiConnectDeferred": 1,
            "reconnects": 0,
            "disconnects": 0,
            "heapDmaMin": 25000,
            "heapDmaLargestMin": 14000,
            "bleProcessMaxUs": 40000,
            "bleMutexTimeout": 0,
            "gpsObsDrops": 0,
            "proxy": {"dropCount": 0, "advertising": 1},
            "eventBus": {"publishCount": 20, "dropCount": 0, "size": 64},
            "lockout": {"coreGuardTripped": False},
        },
    ]
    for index, data in enumerate(samples):
        if not include_display_counters:
            data = dict(data)
            data.pop("displayUpdates", None)
            data.pop("displaySkips", None)
        records.append(
            {
                "ts": (start + timedelta(seconds=index)).isoformat().replace("+00:00", "Z"),
                "ok": True,
                "data": data,
            }
        )
    path.write_text("".join(json.dumps(item) + "\n" for item in records), encoding="utf-8")


def write_panic_jsonl(path: Path, *, runtime_crash: bool = False) -> None:
    start = datetime(2026, 3, 12, 12, 0, 0, tzinfo=timezone.utc)
    samples = [
        {
            "ts": start.isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "wasCrash": False,
                "hasPanicFile": False,
                "lastResetReason": "boot",
            },
        },
        {
            "ts": (start + timedelta(seconds=1)).isoformat().replace("+00:00", "Z"),
            "ok": True,
            "data": {
                "wasCrash": runtime_crash,
                "hasPanicFile": runtime_crash,
                "lastResetReason": "panic" if runtime_crash else "steady",
            },
        },
    ]
    path.write_text("".join(json.dumps(item) + "\n" for item in samples), encoding="utf-8")


def write_serial_log(path: Path, *, reset_count: int = 0, panic_signature_count: int = 0) -> None:
    lines = ["I (0) boot: firmware ready"]
    for _ in range(reset_count):
        lines.append("rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)")
    for _ in range(panic_signature_count):
        lines.append("Guru Meditation Error: Core 0 panic'ed (LoadProhibited).")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_test_script(env_overrides: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.update(env_overrides)
    return subprocess.run(
        [str(TEST_SCRIPT), *args],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp_dir:
        temp_root = Path(tmp_dir)
        inventory = temp_root / "board_inventory.json"
        write_json(
            inventory,
            {
                "description": "fake inventory",
                "boards": [
                    {
                        "board_id": "release",
                        "device_path": "/dev/test-release",
                        "metrics_url": "http://127.0.0.1:9999/api/debug/metrics",
                    },
                    {
                        "board_id": "radio",
                        "device_path": "/dev/test-radio",
                        "metrics_url": "http://127.0.0.1:9998/api/debug/metrics",
                    }
                ],
            },
        )

        fake_device = temp_root / "fake_device_tests.sh"
        fake_soak = temp_root / "fake_real_soak.sh"
        create_fake_child_script(fake_device, "device")
        create_fake_child_script(fake_soak, "soak")

        artifact_root = temp_root / "artifacts"

        common_env = {
            "HARDWARE_TEST_INVENTORY": str(inventory),
            "HARDWARE_TEST_DEVICE_TESTS_SCRIPT": str(fake_device),
            "HARDWARE_TEST_REAL_SOAK_SCRIPT": str(fake_soak),
            "HARDWARE_TEST_ARTIFACT_ROOT": str(artifact_root),
            "HARDWARE_TEST_SOAK_DURATION_SECONDS": "1",
            "HARDWARE_TEST_HTTP_TIMEOUT_SECONDS": "1",
            "HARDWARE_TEST_METRICS_ENDPOINT_ATTEMPTS": "1",
            "HARDWARE_TEST_METRICS_ENDPOINT_RETRY_DELAY_SECONDS": "0",
        }
        release_root = artifact_root / "release"
        radio_root = artifact_root / "radio"

        first = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "NO_BASELINE",
                "FAKE_DEVICE_COMPARE_KIND": "no_baseline",
                "FAKE_DEVICE_VALUE": "10",
                "FAKE_CORE_RESULT": "NO_BASELINE",
                "FAKE_CORE_COMPARE_KIND": "no_baseline",
                "FAKE_CORE_VALUE": "20",
                "FAKE_DISPLAY_RESULT": "NO_BASELINE",
                "FAKE_DISPLAY_COMPARE_KIND": "no_baseline",
                "FAKE_DISPLAY_VALUE": "30",
            }
        )
        assert_true(first.returncode == 0, f"first hardware test failed: {first.stdout}\n{first.stderr}")

        latest = release_root / "latest"
        first_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        first_run_dir = Path(first_result["run_dir"])
        assert_true(first_result["result"] == "NO_BASELINE", f"unexpected first result: {first_result}")
        assert_true(first_result["board_id"] == "release", f"unexpected first board id: {first_result}")
        assert_true(first_result["previous_run_dir"] == "", "first run should not have previous_run_dir")
        assert_true((latest / "comparison.txt").exists(), "suite comparison.txt missing")
        assert_true((latest / "device_tests" / "comparison.txt").exists(), "device comparison.txt missing")
        assert_true((latest / "core_soak" / "comparison.txt").exists(), "core comparison.txt missing")
        assert_true((latest / "display_soak" / "comparison.txt").exists(), "display comparison.txt missing")

        radio = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "NO_BASELINE",
                "FAKE_DEVICE_COMPARE_KIND": "no_baseline",
                "FAKE_DEVICE_VALUE": "40",
                "FAKE_CORE_RESULT": "NO_BASELINE",
                "FAKE_CORE_COMPARE_KIND": "no_baseline",
                "FAKE_CORE_VALUE": "50",
                "FAKE_DISPLAY_RESULT": "NO_BASELINE",
                "FAKE_DISPLAY_COMPARE_KIND": "no_baseline",
                "FAKE_DISPLAY_VALUE": "60",
            },
            "--board-id",
            "radio",
        )
        assert_true(radio.returncode == 0, f"radio hardware test failed: {radio.stdout}\n{radio.stderr}")

        radio_latest = radio_root / "latest"
        radio_result = json.loads((radio_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(radio_result["board_id"] == "radio", f"unexpected radio board id: {radio_result}")
        assert_true(radio_result["previous_run_dir"] == "", "first radio run should not have previous_run_dir")

        release_after_radio = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(release_after_radio["run_dir"] == str(first_run_dir), "radio run should not move release latest")

        second = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "11",
                "FAKE_DEVICE_BASELINE": "10",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "commit_regression",
                "FAKE_CORE_VALUE": "21",
                "FAKE_CORE_BASELINE": "20",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "31",
                "FAKE_DISPLAY_BASELINE": "30",
            }
        )
        assert_true(second.returncode == 0, f"second hardware test failed: {second.stdout}\n{second.stderr}")

        second_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(second_result["result"] == "PASS", f"unexpected second result: {second_result}")
        assert_true(second_result["previous_run_dir"] == str(first_run_dir), "suite did not point to previous run dir")

        device_compare = (latest / "device_tests" / "received_compare_to.txt").read_text(encoding="utf-8").strip()
        core_compare = (latest / "core_soak" / "received_compare_to.txt").read_text(encoding="utf-8").strip()
        display_compare = (latest / "display_soak" / "received_compare_to.txt").read_text(encoding="utf-8").strip()
        assert_true(device_compare.endswith("/device_tests/manifest.json"), f"unexpected device compare path: {device_compare}")
        assert_true(core_compare.endswith("/core_soak/manifest.json"), f"unexpected core compare path: {core_compare}")
        assert_true(display_compare.endswith("/display_soak/manifest.json"), f"unexpected display compare path: {display_compare}")

        run_history = read_tsv(release_root / "run_history.tsv")
        assert_true(len(run_history) == 2, f"expected 2 run-history rows, saw {len(run_history)}")
        assert_true(run_history[-1]["result"] == "PASS", f"unexpected run-history row: {run_history[-1]}")
        assert_true(run_history[-1]["warning_policy"] == "non_blocking", f"unexpected warning policy: {run_history[-1]}")

        metric_history = read_tsv(release_root / "metric_history.tsv")
        assert_true(len(metric_history) == 6, f"expected 6 metric-history rows, saw {len(metric_history)}")

        radio_run_history = read_tsv(radio_root / "run_history.tsv")
        assert_true(len(radio_run_history) == 1, f"expected 1 radio run-history row, saw {len(radio_run_history)}")
        radio_metric_history = read_tsv(radio_root / "metric_history.tsv")
        assert_true(len(radio_metric_history) == 3, f"expected 3 radio metric-history rows, saw {len(radio_metric_history)}")

        suite_comparison = (latest / "comparison.txt").read_text(encoding="utf-8")
        assert_true("device_tests" in suite_comparison, "suite comparison missing device_tests row")
        assert_true("commit_regression" in suite_comparison, "suite comparison missing compare kind")
        assert_true("warning_policy: non_blocking" in suite_comparison, "suite comparison missing warning policy")

        device_only = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "run_variance",
                "FAKE_DEVICE_VALUE": "12",
            },
            "--device",
        )
        assert_true(device_only.returncode == 0, f"device-only hardware test failed: {device_only.stdout}\n{device_only.stderr}")
        device_only_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true([step["name"] for step in device_only_result["steps"]] == ["device_tests"], f"unexpected device-only steps: {device_only_result}")

        warning_artifact_root = temp_root / "warning_artifacts"
        warning_env = {
            **common_env,
            "HARDWARE_TEST_ARTIFACT_ROOT": str(warning_artifact_root),
            "FAKE_DEVICE_RESULT": "PASS_WITH_WARNINGS",
            "FAKE_DEVICE_COMPARE_KIND": "run_variance",
            "FAKE_DEVICE_VALUE": "13",
        }
        warning_non_strict = run_test_script(warning_env, "--device")
        assert_true(
            warning_non_strict.returncode == 0,
            f"non-strict warning run should pass: {warning_non_strict.stdout}\n{warning_non_strict.stderr}",
        )
        warning_latest = warning_artifact_root / "release" / "latest"
        warning_result = json.loads((warning_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(warning_result["result"] == "PASS_WITH_WARNINGS", f"unexpected warning result: {warning_result}")
        assert_true(warning_result["warning_policy"] == "non_blocking", f"unexpected warning policy: {warning_result}")

        warning_strict = run_test_script(warning_env, "--device", "--strict")
        assert_true(
            warning_strict.returncode == 1,
            f"strict warning run should fail: {warning_strict.stdout}\n{warning_strict.stderr}",
        )
        strict_result = json.loads((warning_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(strict_result["warning_policy"] == "blocking", f"unexpected strict warning policy: {strict_result}")

        parse_artifact_root = temp_root / "parse_artifacts"
        parse_input_dir = temp_root / "captured_drive"
        parse_input_dir.mkdir(parents=True, exist_ok=True)
        write_metrics_jsonl(parse_input_dir / "metrics.jsonl")
        write_panic_jsonl(parse_input_dir / "panic.jsonl")
        write_serial_log(parse_input_dir / "serial.log")

        parsed_display = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(parse_artifact_root),
            },
            "--display",
            "--parse-drive-log",
            str(parse_input_dir),
        )
        assert_true(parsed_display.returncode == 0, f"parsed display hardware test failed: {parsed_display.stdout}\n{parsed_display.stderr}")

        parse_latest = parse_artifact_root / "release" / "latest"
        parsed_result = json.loads((parse_latest / "result.json").read_text(encoding="utf-8"))
        assert_true([step["name"] for step in parsed_result["steps"]] == ["display_soak"], f"unexpected parsed steps: {parsed_result}")
        parsed_scoring = json.loads((parse_latest / "display_soak" / "scoring.json").read_text(encoding="utf-8"))
        assert_true(parsed_scoring["result"] in {"NO_BASELINE", "PASS", "PASS_WITH_WARNINGS"}, f"unexpected parsed scoring: {parsed_scoring}")
        parsed_comparison = (parse_latest / "display_soak" / "comparison.txt").read_text(encoding="utf-8")
        assert_true("display_updates_delta" in parsed_comparison, "parsed comparison missing display metric")
        assert_true("## Imported Signals" in parsed_comparison, "parsed comparison missing imported signals section")
        assert_true("Reboot evidence detected: no" in parsed_comparison, "parsed comparison should show no reboot evidence")
        parsed_manifest = json.loads((parse_latest / "display_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(parsed_manifest["source_serial_log"].endswith("/serial.log"), f"missing serial source: {parsed_manifest}")
        assert_true(parsed_manifest["source_panic_jsonl"].endswith("/panic.jsonl"), f"missing panic source: {parsed_manifest}")
        import_diagnostics = json.loads((parse_latest / "display_soak" / "import_diagnostics.json").read_text(encoding="utf-8"))
        assert_true(import_diagnostics["source_coverage"]["signal_sources"] == 3, f"unexpected signal coverage: {import_diagnostics}")

        serial_only_artifact_root = temp_root / "serial_only_artifacts"
        serial_only_dir = temp_root / "serial_only_drive"
        serial_only_dir.mkdir(parents=True, exist_ok=True)
        write_serial_log(serial_only_dir / "serial.log")
        serial_only = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(serial_only_artifact_root),
            },
            "--core",
            "--parse-drive-log",
            str(serial_only_dir / "serial.log"),
        )
        assert_true(serial_only.returncode == 0, f"serial-only parse should be non-failing: {serial_only.stdout}\n{serial_only.stderr}")
        serial_only_latest = serial_only_artifact_root / "release" / "latest"
        serial_only_result = json.loads((serial_only_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(serial_only_result["result"] == "PASS_WITH_WARNINGS", f"unexpected serial-only result: {serial_only_result}")
        serial_only_step_manifest = json.loads((serial_only_latest / "core_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(serial_only_step_manifest["result"] == "INCONCLUSIVE", f"serial-only manifest should be inconclusive: {serial_only_step_manifest}")
        serial_only_comparison = (serial_only_latest / "core_soak" / "comparison.txt").read_text(encoding="utf-8")
        assert_true("Metrics JSONL: missing" in serial_only_comparison, "serial-only comparison should note missing metrics")

        crash_artifact_root = temp_root / "crash_artifacts"
        crash_input_dir = temp_root / "crash_drive"
        crash_input_dir.mkdir(parents=True, exist_ok=True)
        write_metrics_jsonl(crash_input_dir / "metrics.jsonl", include_display_counters=False)
        write_panic_jsonl(crash_input_dir / "panic.jsonl", runtime_crash=True)
        write_serial_log(crash_input_dir / "serial.log", panic_signature_count=1)
        parsed_crash = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(crash_artifact_root),
            },
            "--core",
            "--parse-drive-log",
            str(crash_input_dir),
        )
        assert_true(parsed_crash.returncode == 1, f"crash parse should fail: {parsed_crash.stdout}\n{parsed_crash.stderr}")
        crash_latest = crash_artifact_root / "release" / "latest"
        crash_result = json.loads((crash_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(crash_result["result"] == "FAIL", f"unexpected crash result: {crash_result}")
        crash_comparison = (crash_latest / "core_soak" / "comparison.txt").read_text(encoding="utf-8")
        assert_true("Reboot evidence detected: yes" in crash_comparison, "crash comparison should note reboot evidence")
        assert_true("serial_panic_signatures=1" in crash_comparison, "crash comparison missing serial panic detail")

    print("hardware test script regression tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
