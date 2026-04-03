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


def read_lines(path: Path) -> list[str]:
    if not path.exists():
        return []
    return [line for line in path.read_text(encoding="utf-8").splitlines() if line]


def resolved_path_text(path: str | Path) -> str:
    return str(Path(path).resolve())


def create_fake_child_script(path: Path, child_type: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"""#!/usr/bin/env bash
set -euo pipefail

OUT_DIR=""
COMPARE_TO=()
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
      COMPARE_TO+=("$2")
      shift
      ;;
    --drive-display-preview)
      STEP_KIND="display"
      ;;
  esac
  shift
done

mkdir -p "$OUT_DIR"
if [[ "${{#COMPARE_TO[@]}}" -gt 0 ]]; then
  printf "%s\\n" "${{COMPARE_TO[@]}}" > "$OUT_DIR/received_compare_to.txt"
fi

STEP_UPPER="$(printf "%s" "$STEP_KIND" | tr '[:lower:]' '[:upper:]')"
RESULT_VAR="FAKE_${{STEP_UPPER}}_RESULT"
COMPARE_VAR="FAKE_${{STEP_UPPER}}_COMPARE_KIND"
VALUE_VAR="FAKE_${{STEP_UPPER}}_VALUE"
BASELINE_VAR="FAKE_${{STEP_UPPER}}_BASELINE"
EXIT_VAR="FAKE_${{STEP_UPPER}}_EXIT"

FIRST_COMPARE_TO="${{COMPARE_TO[0]:-}}"
COMPARE_TO_COUNT="${{#COMPARE_TO[@]}}"
python3 - "$OUT_DIR" "$FIRST_COMPARE_TO" "$COMPARE_TO_COUNT" "$STEP_KIND" "$BOARD_ID" "${{!RESULT_VAR}}" "${{!COMPARE_VAR}}" "${{!VALUE_VAR}}" "${{!BASELINE_VAR:-}}" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

out_dir = Path(sys.argv[1])
compare_to = sys.argv[2]
compare_to_count = int(sys.argv[3])
step_kind = sys.argv[4]
board_id = sys.argv[5]
result = sys.argv[6]
compare_kind = sys.argv[7]
value = float(sys.argv[8])
baseline_raw = sys.argv[9]
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

baseline_window = {{
    "strategy": "single_baseline" if compare_to_count == 1 else ("median_last_3_trustworthy" if compare_to_count > 1 else "none"),
    "candidate_count": compare_to_count,
    "candidates": [],
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
    "baseline_window": baseline_window,
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

exit "${{!EXIT_VAR:-0}}"
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
            "proxy": {"dropCount": 0, "advertising": 1},
            "eventBus": {"publishCount": 0, "dropCount": 0, "size": 64},
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
            "proxy": {"dropCount": 0, "advertising": 1},
            "eventBus": {"publishCount": 10, "dropCount": 0, "size": 64},
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
            "proxy": {"dropCount": 0, "advertising": 1},
            "eventBus": {"publishCount": 20, "dropCount": 0, "size": 64},
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


HEADER_COLUMNS = [
    line.strip()
    for line in (ROOT / "test" / "contracts" / "perf_csv_column_contract.txt").read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.startswith("#")
]
LEGACY_HEADER_COLUMNS = HEADER_COLUMNS[:-4]
# Optional direct-speed extension used for importer compatibility tests.
DRIVE_HEADER_COLUMNS = HEADER_COLUMNS + ["obdSpeedMph_x10"]


def _base_csv_row(millis: int, *, header_columns: list[str], connected: bool = True, drive_like: bool = False) -> dict[str, int]:
    row = {column: 0 for column in header_columns}
    row.update(
        {
            "millis": millis,
            "timeValid": 1,
            "timeSource": 1,
            "loopMax_us": 100000,
            "bleDrainMax_us": 4000,
            "bleProcessMax_us": 40000,
            "dispPipeMax_us": 30000,
            "flushMax_us": 40000,
            "sdMax_us": 20000,
            "fsMax_us": 20000,
            "queueHighWater": 5,
            "freeDma": 30000,
            "largestDma": 18000,
            "dmaLargestMin": 15000,
            "dmaFreeMin": 30000,
            "wifiMax_us": 500,
            "rx": 0,
            "parseOK": 0,
        }
    )
    if "freeDmaMin" in row:
        row["freeDmaMin"] = 26000
    if "largestDmaMin" in row:
        row["largestDmaMin"] = 15000
    if "perfDrop" in row:
        row["perfDrop"] = 0
    if "eventBusDrops" in row:
        row["eventBusDrops"] = 0
    if connected:
        row["rx"] = 120
        row["parseOK"] = 120
    if drive_like and "obdSpeedMph_x10" in header_columns:
        row["obdSpeedMph_x10"] = 350
    return row


def write_perf_csv(
    path: Path,
    *,
    duration_ms: int = 60000,
    rows: int = 5,
    header_columns: list[str] | None = None,
    sessions: list[dict[str, object]] | None = None,
    leading_rows: list[dict[str, int]] | None = None,
) -> None:
    fieldnames = header_columns or HEADER_COLUMNS
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        if leading_rows:
            for row in leading_rows:
                writer.writerow({key: row.get(key, 0) for key in fieldnames})
        if sessions is None:
            sessions = [
                {
                    "meta": f"#session_start,seq=1,bootId=1,uptime_ms={duration_ms},token=TEST0001,schema={13 if fieldnames == HEADER_COLUMNS else 12}",
                    "rows": [],
                }
            ]
            for i in range(rows):
                frac = i / max(rows - 1, 1)
                row = _base_csv_row(int(frac * duration_ms), header_columns=fieldnames, connected=True, drive_like=True)
                row["rx"] = 120 + i * 40
                row["parseOK"] = 120 + i * 40
                row["displayUpdates"] = i * 10
                sessions[0]["rows"].append(row)

        for session in sessions:
            writer.writeheader()
            handle.write(f"{session['meta']}\n")
            for row in session["rows"]:
                writer.writerow({key: row.get(key, 0) for key in fieldnames})


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
        second_run_dir = Path(second_result["run_dir"])
        assert_true(second_result["result"] == "PASS", f"unexpected second result: {second_result}")
        assert_true(second_result["previous_run_dir"] == str(first_run_dir), "suite did not point to previous run dir")

        device_compare = read_lines(latest / "device_tests" / "received_compare_to.txt")
        core_compare = read_lines(latest / "core_soak" / "received_compare_to.txt")
        display_compare = read_lines(latest / "display_soak" / "received_compare_to.txt")
        assert_true(len(device_compare) == 1, f"unexpected device compare list: {device_compare}")
        assert_true(len(core_compare) == 1, f"unexpected core compare list: {core_compare}")
        assert_true(len(display_compare) == 1, f"unexpected display compare list: {display_compare}")
        assert_true(device_compare[0].endswith("/device_tests/manifest.json"), f"unexpected device compare path: {device_compare}")
        assert_true(core_compare[0].endswith("/core_soak/manifest.json"), f"unexpected core compare path: {core_compare}")
        assert_true(display_compare[0].endswith("/display_soak/manifest.json"), f"unexpected display compare path: {display_compare}")

        run_history = read_tsv(release_root / "run_history.tsv")
        assert_true(len(run_history) == 2, f"expected 2 run-history rows, saw {len(run_history)}")
        assert_true(run_history[-1]["result"] == "PASS", f"unexpected run-history row: {run_history[-1]}")
        assert_true(run_history[-1]["warning_policy"] == "non_blocking", f"unexpected warning policy: {run_history[-1]}")

        metric_history = read_tsv(release_root / "metric_history.tsv")
        assert_true(len(metric_history) == 6, f"expected 6 metric-history rows, saw {len(metric_history)}")

        failing = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "FAIL",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "12",
                "FAKE_DEVICE_BASELINE": "11",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "commit_regression",
                "FAKE_CORE_VALUE": "22",
                "FAKE_CORE_BASELINE": "21",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "32",
                "FAKE_DISPLAY_BASELINE": "31",
            }
        )
        assert_true(failing.returncode == 1, f"failing hardware test should exit 1: {failing.stdout}\n{failing.stderr}")
        failing_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        failing_run_dir = Path(failing_result["run_dir"])

        recovered = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "13",
                "FAKE_DEVICE_BASELINE": "11",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "commit_regression",
                "FAKE_CORE_VALUE": "23",
                "FAKE_CORE_BASELINE": "21",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "33",
                "FAKE_DISPLAY_BASELINE": "31",
            }
        )
        assert_true(recovered.returncode == 0, f"recovered hardware test failed: {recovered.stdout}\n{recovered.stderr}")

        recovered_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(recovered_result["result"] == "PASS", f"unexpected recovered result: {recovered_result}")
        assert_true(
            recovered_result["previous_run_dir"] == str(second_run_dir),
            f"recovered run should skip failed latest baseline: {recovered_result}",
        )

        recovered_device_compare = read_lines(latest / "device_tests" / "received_compare_to.txt")
        recovered_core_compare = read_lines(latest / "core_soak" / "received_compare_to.txt")
        recovered_display_compare = read_lines(latest / "display_soak" / "received_compare_to.txt")
        assert_true(
            len(recovered_device_compare) == 1
            and resolved_path_text(recovered_device_compare[0]).startswith(resolved_path_text(second_run_dir / "device_tests")),
            f"unexpected recovered device compare path: {recovered_device_compare}",
        )
        assert_true(
            [resolved_path_text(item) for item in recovered_core_compare] == [
                resolved_path_text(failing_run_dir / "core_soak" / "manifest.json"),
                resolved_path_text(second_run_dir / "core_soak" / "manifest.json"),
            ],
            f"unexpected recovered core compare path: {recovered_core_compare}",
        )
        assert_true(
            [resolved_path_text(item) for item in recovered_display_compare] == [
                resolved_path_text(failing_run_dir / "display_soak" / "manifest.json"),
                resolved_path_text(second_run_dir / "display_soak" / "manifest.json"),
            ],
            f"unexpected recovered display compare path: {recovered_display_compare}",
        )

        window_artifact_root = temp_root / "window_artifacts"
        window_env = {
            **common_env,
            "HARDWARE_TEST_ARTIFACT_ROOT": str(window_artifact_root),
        }
        window_latest = window_artifact_root / "release" / "latest"

        window_seed_1 = run_test_script(
            {
                **window_env,
                "FAKE_DEVICE_RESULT": "NO_BASELINE",
                "FAKE_DEVICE_COMPARE_KIND": "no_baseline",
                "FAKE_DEVICE_VALUE": "101",
                "FAKE_CORE_RESULT": "NO_BASELINE",
                "FAKE_CORE_COMPARE_KIND": "no_baseline",
                "FAKE_CORE_VALUE": "201",
                "FAKE_DISPLAY_RESULT": "NO_BASELINE",
                "FAKE_DISPLAY_COMPARE_KIND": "no_baseline",
                "FAKE_DISPLAY_VALUE": "301",
            }
        )
        assert_true(window_seed_1.returncode == 0, f"window seed 1 failed: {window_seed_1.stdout}\n{window_seed_1.stderr}")
        window_seed_1_result = json.loads((window_latest / "result.json").read_text(encoding="utf-8"))
        window_seed_1_dir = Path(window_seed_1_result["run_dir"])

        window_seed_2 = run_test_script(
            {
                **window_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "run_variance",
                "FAKE_DEVICE_VALUE": "102",
                "FAKE_DEVICE_BASELINE": "101",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "202",
                "FAKE_CORE_BASELINE": "201",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "run_variance",
                "FAKE_DISPLAY_VALUE": "302",
                "FAKE_DISPLAY_BASELINE": "301",
            }
        )
        assert_true(window_seed_2.returncode == 0, f"window seed 2 failed: {window_seed_2.stdout}\n{window_seed_2.stderr}")
        window_seed_2_result = json.loads((window_latest / "result.json").read_text(encoding="utf-8"))
        window_seed_2_dir = Path(window_seed_2_result["run_dir"])

        window_seed_3 = run_test_script(
            {
                **window_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "run_variance",
                "FAKE_DEVICE_VALUE": "103",
                "FAKE_DEVICE_BASELINE": "102",
                "FAKE_CORE_RESULT": "FAIL",
                "FAKE_CORE_COMPARE_KIND": "commit_regression",
                "FAKE_CORE_VALUE": "203",
                "FAKE_CORE_BASELINE": "202",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "run_variance",
                "FAKE_DISPLAY_VALUE": "303",
                "FAKE_DISPLAY_BASELINE": "302",
            }
        )
        assert_true(window_seed_3.returncode == 0, f"window seed 3 failed: {window_seed_3.stdout}\n{window_seed_3.stderr}")
        window_seed_3_result = json.loads((window_latest / "result.json").read_text(encoding="utf-8"))
        window_seed_3_dir = Path(window_seed_3_result["run_dir"])

        window_seed_4 = run_test_script(
            {
                **window_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "run_variance",
                "FAKE_DEVICE_VALUE": "104",
                "FAKE_DEVICE_BASELINE": "103",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "204",
                "FAKE_CORE_BASELINE": "202",
                "FAKE_DISPLAY_RESULT": "FAIL",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "304",
                "FAKE_DISPLAY_BASELINE": "303",
            }
        )
        assert_true(window_seed_4.returncode == 0, f"window seed 4 failed: {window_seed_4.stdout}\n{window_seed_4.stderr}")
        window_seed_4_result = json.loads((window_latest / "result.json").read_text(encoding="utf-8"))
        window_seed_4_dir = Path(window_seed_4_result["run_dir"])

        window_probe = run_test_script(
            {
                **window_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "run_variance",
                "FAKE_DEVICE_VALUE": "105",
                "FAKE_DEVICE_BASELINE": "104",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "205",
                "FAKE_CORE_BASELINE": "204",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "run_variance",
                "FAKE_DISPLAY_VALUE": "305",
                "FAKE_DISPLAY_BASELINE": "303",
            }
        )
        assert_true(window_probe.returncode == 0, f"window probe failed: {window_probe.stdout}\n{window_probe.stderr}")
        window_probe_result = json.loads((window_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(
            window_probe_result["previous_run_dir"] == str(window_seed_2_dir),
            f"suite previous_run_dir should stay on newest fully trustworthy run: {window_probe_result}",
        )

        device_window_compare = read_lines(window_latest / "device_tests" / "received_compare_to.txt")
        core_window_compare = read_lines(window_latest / "core_soak" / "received_compare_to.txt")
        display_window_compare = read_lines(window_latest / "display_soak" / "received_compare_to.txt")
        assert_true(
            [resolved_path_text(item) for item in device_window_compare] == [
                resolved_path_text(window_seed_4_dir / "device_tests" / "manifest.json"),
                resolved_path_text(window_seed_3_dir / "device_tests" / "manifest.json"),
                resolved_path_text(window_seed_2_dir / "device_tests" / "manifest.json"),
            ],
            f"device baseline window should forward last 3 passing manifests newest-first: {device_window_compare}",
        )
        assert_true(
            [resolved_path_text(item) for item in core_window_compare] == [
                resolved_path_text(window_seed_4_dir / "core_soak" / "manifest.json"),
                resolved_path_text(window_seed_2_dir / "core_soak" / "manifest.json"),
            ],
            f"core baseline window should exclude failed run and NO_BASELINE fallback when passes exist: {core_window_compare}",
        )
        assert_true(
            [resolved_path_text(item) for item in display_window_compare] == [
                resolved_path_text(window_seed_3_dir / "display_soak" / "manifest.json"),
                resolved_path_text(window_seed_2_dir / "display_soak" / "manifest.json"),
            ],
            f"display baseline window should exclude failed run and preserve newest-first ordering: {display_window_compare}",
        )

        baseline_override_path = window_artifact_root / "release" / "baseline_manifest_overrides.json"
        write_json(
            baseline_override_path,
            {
                "schema_version": 1,
                "board_id": "release",
                "previous_run_dir": str(window_seed_4_dir),
                "steps": {
                    "device_tests": [
                        str(window_seed_2_dir / "device_tests" / "manifest.json"),
                        str(window_seed_1_dir / "device_tests" / "manifest.json"),
                    ],
                    "core_soak": [
                        str(window_seed_2_dir / "core_soak" / "manifest.json"),
                    ],
                    "display_soak": [
                        str(window_seed_3_dir / "display_soak" / "manifest.json"),
                        str(window_seed_2_dir / "display_soak" / "manifest.json"),
                    ],
                },
            },
        )

        pinned_probe = run_test_script(
            {
                **window_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "run_variance",
                "FAKE_DEVICE_VALUE": "106",
                "FAKE_DEVICE_BASELINE": "105",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "206",
                "FAKE_CORE_BASELINE": "205",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "run_variance",
                "FAKE_DISPLAY_VALUE": "306",
                "FAKE_DISPLAY_BASELINE": "305",
            }
        )
        assert_true(pinned_probe.returncode == 0, f"pinned probe failed: {pinned_probe.stdout}\n{pinned_probe.stderr}")
        pinned_probe_result = json.loads((window_latest / "result.json").read_text(encoding="utf-8"))
        assert_true(
            resolved_path_text(pinned_probe_result["previous_run_dir"]) == resolved_path_text(window_seed_4_dir),
            f"pinned previous_run_dir should be used when baseline override exists: {pinned_probe_result}",
        )

        device_pinned_compare = read_lines(window_latest / "device_tests" / "received_compare_to.txt")
        core_pinned_compare = read_lines(window_latest / "core_soak" / "received_compare_to.txt")
        display_pinned_compare = read_lines(window_latest / "display_soak" / "received_compare_to.txt")
        assert_true(
            [resolved_path_text(item) for item in device_pinned_compare] == [
                resolved_path_text(window_seed_2_dir / "device_tests" / "manifest.json"),
                resolved_path_text(window_seed_1_dir / "device_tests" / "manifest.json"),
            ],
            f"device baseline override should replace automatic window selection: {device_pinned_compare}",
        )
        assert_true(
            [resolved_path_text(item) for item in core_pinned_compare] == [
                resolved_path_text(window_seed_2_dir / "core_soak" / "manifest.json"),
            ],
            f"core baseline override should replace automatic window selection: {core_pinned_compare}",
        )
        assert_true(
            [resolved_path_text(item) for item in display_pinned_compare] == [
                resolved_path_text(window_seed_3_dir / "display_soak" / "manifest.json"),
                resolved_path_text(window_seed_2_dir / "display_soak" / "manifest.json"),
            ],
            f"display baseline override should replace automatic window selection: {display_pinned_compare}",
        )

        soak_only_failure = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "14",
                "FAKE_DEVICE_BASELINE": "13",
                "FAKE_CORE_RESULT": "FAIL",
                "FAKE_CORE_COMPARE_KIND": "commit_regression",
                "FAKE_CORE_VALUE": "24",
                "FAKE_CORE_BASELINE": "23",
                "FAKE_DISPLAY_RESULT": "FAIL",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "34",
                "FAKE_DISPLAY_BASELINE": "33",
            }
        )
        assert_true(
            soak_only_failure.returncode == 0,
            f"device-owned suite verdict should ignore soak-only failures: {soak_only_failure.stdout}\n{soak_only_failure.stderr}",
        )

        soak_only_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(soak_only_result["result"] == "PASS", f"unexpected soak-only suite result: {soak_only_result}")
        assert_true(
            soak_only_result["authoritative_steps"] == ["device_tests"],
            f"unexpected authoritative steps: {soak_only_result}",
        )
        assert_true(
            soak_only_result["diagnostic_failures"] == [
                {"name": "core_soak", "result": "FAIL"},
                {"name": "display_soak", "result": "FAIL"},
            ],
            f"unexpected diagnostic failures: {soak_only_result}",
        )
        assert_true(
            soak_only_result["diagnostic_warnings"] == [],
            f"unexpected diagnostic warnings: {soak_only_result}",
        )
        assert_true(
            "diagnostic failures: core_soak FAIL, display_soak FAIL" in soak_only_result["rollup_summary"],
            f"unexpected rollup summary: {soak_only_result}",
        )
        soak_step_results = {step["name"]: step["result"] for step in soak_only_result["steps"]}
        assert_true(soak_step_results["device_tests"] == "PASS", f"device step should remain pass: {soak_only_result}")
        assert_true(soak_step_results["core_soak"] == "FAIL", f"core soak failure should still be recorded: {soak_only_result}")
        assert_true(
            soak_step_results["display_soak"] == "FAIL",
            f"display soak failure should still be recorded: {soak_only_result}",
        )

        soak_only_warning = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "16",
                "FAKE_DEVICE_BASELINE": "15",
                "FAKE_CORE_RESULT": "PASS_WITH_WARNINGS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "26",
                "FAKE_CORE_BASELINE": "25",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "36",
                "FAKE_DISPLAY_BASELINE": "35",
            }
        )
        assert_true(
            soak_only_warning.returncode == 0,
            f"default suite should keep soak warnings diagnostic: {soak_only_warning.stdout}\n{soak_only_warning.stderr}",
        )
        soak_warning_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(soak_warning_result["result"] == "PASS", f"unexpected soak-warning suite result: {soak_warning_result}")
        assert_true(
            soak_warning_result["diagnostic_warnings"] == [{"name": "core_soak", "result": "PASS_WITH_WARNINGS"}],
            f"unexpected diagnostic warnings: {soak_warning_result}",
        )
        assert_true(
            "diagnostic warnings: core_soak PASS_WITH_WARNINGS" in soak_warning_result["rollup_summary"],
            f"unexpected soak warning summary: {soak_warning_result}",
        )

        soak_warning_strict = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "16",
                "FAKE_DEVICE_BASELINE": "15",
                "FAKE_CORE_RESULT": "PASS_WITH_WARNINGS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "26",
                "FAKE_CORE_BASELINE": "25",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "36",
                "FAKE_DISPLAY_BASELINE": "35",
            },
            "--strict",
        )
        assert_true(
            soak_warning_strict.returncode == 0,
            f"--strict alone should not promote non-authoritative soak warnings: {soak_warning_strict.stdout}\n{soak_warning_strict.stderr}",
        )

        strict_soaks_fail = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "17",
                "FAKE_DEVICE_BASELINE": "16",
                "FAKE_CORE_RESULT": "FAIL",
                "FAKE_CORE_COMPARE_KIND": "commit_regression",
                "FAKE_CORE_VALUE": "27",
                "FAKE_CORE_BASELINE": "26",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "37",
                "FAKE_DISPLAY_BASELINE": "36",
            },
            "--strict-soaks",
        )
        assert_true(
            strict_soaks_fail.returncode == 1,
            f"--strict-soaks should make soak failures authoritative: {strict_soaks_fail.stdout}\n{strict_soaks_fail.stderr}",
        )
        strict_soaks_fail_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(strict_soaks_fail_result["result"] == "FAIL", f"unexpected strict-soaks fail result: {strict_soaks_fail_result}")
        assert_true(
            strict_soaks_fail_result["authoritative_steps"] == ["device_tests", "core_soak", "display_soak"],
            f"unexpected strict-soaks authority: {strict_soaks_fail_result}",
        )
        assert_true(
            strict_soaks_fail_result["diagnostic_failures"] == [],
            f"strict-soaks run should not report authoritative failures as diagnostics: {strict_soaks_fail_result}",
        )

        strict_soaks_warning = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "18",
                "FAKE_DEVICE_BASELINE": "17",
                "FAKE_CORE_RESULT": "PASS_WITH_WARNINGS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "28",
                "FAKE_CORE_BASELINE": "27",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "38",
                "FAKE_DISPLAY_BASELINE": "37",
            },
            "--strict-soaks",
        )
        assert_true(
            strict_soaks_warning.returncode == 0,
            f"--strict-soaks without --strict should allow warning exit 0: {strict_soaks_warning.stdout}\n{strict_soaks_warning.stderr}",
        )
        strict_soaks_warning_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        assert_true(
            strict_soaks_warning_result["result"] == "PASS_WITH_WARNINGS",
            f"unexpected strict-soaks warning result: {strict_soaks_warning_result}",
        )
        assert_true(
            strict_soaks_warning_result["diagnostic_warnings"] == [],
            f"authoritative warnings should not be diagnostic: {strict_soaks_warning_result}",
        )

        strict_soaks_warning_blocking = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "18",
                "FAKE_DEVICE_BASELINE": "17",
                "FAKE_CORE_RESULT": "PASS_WITH_WARNINGS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "28",
                "FAKE_CORE_BASELINE": "27",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "38",
                "FAKE_DISPLAY_BASELINE": "37",
            },
            "--strict-soaks",
            "--strict",
        )
        assert_true(
            strict_soaks_warning_blocking.returncode == 1,
            f"--strict-soaks plus --strict should fail warning-only suite: {strict_soaks_warning_blocking.stdout}\n{strict_soaks_warning_blocking.stderr}",
        )

        soak_warning_exit = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "19",
                "FAKE_DEVICE_BASELINE": "18",
                "FAKE_CORE_RESULT": "PASS_WITH_WARNINGS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "29",
                "FAKE_CORE_BASELINE": "28",
                "FAKE_CORE_EXIT": "1",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "39",
                "FAKE_DISPLAY_BASELINE": "38",
            }
        )
        assert_true(
            soak_warning_exit.returncode == 0,
            f"warning-only soak exit should stay diagnostic: {soak_warning_exit.stdout}\n{soak_warning_exit.stderr}",
        )
        soak_warning_exit_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        soak_warning_exit_steps = {step["name"]: step for step in soak_warning_exit_result["steps"]}
        assert_true(soak_warning_exit_result["result"] == "PASS", f"unexpected suite result: {soak_warning_exit_result}")
        assert_true(
            soak_warning_exit_steps["core_soak"]["exit_code"] == 1,
            f"core soak exit should still be preserved: {soak_warning_exit_result}",
        )
        assert_true(
            soak_warning_exit_steps["core_soak"]["result"] == "PASS_WITH_WARNINGS",
            f"warning-only soak should preserve PASS_WITH_WARNINGS despite non-zero exit: {soak_warning_exit_result}",
        )
        assert_true(
            soak_warning_exit_result["diagnostic_warnings"] == [{"name": "core_soak", "result": "PASS_WITH_WARNINGS"}],
            f"warning-only soak should remain a diagnostic warning: {soak_warning_exit_result}",
        )

        soak_exit_mismatch = run_test_script(
            {
                **common_env,
                "FAKE_DEVICE_RESULT": "PASS",
                "FAKE_DEVICE_COMPARE_KIND": "commit_regression",
                "FAKE_DEVICE_VALUE": "15",
                "FAKE_DEVICE_BASELINE": "14",
                "FAKE_CORE_RESULT": "PASS",
                "FAKE_CORE_COMPARE_KIND": "run_variance",
                "FAKE_CORE_VALUE": "25",
                "FAKE_CORE_BASELINE": "24",
                "FAKE_CORE_EXIT": "1",
                "FAKE_DISPLAY_RESULT": "PASS",
                "FAKE_DISPLAY_COMPARE_KIND": "commit_regression",
                "FAKE_DISPLAY_VALUE": "35",
                "FAKE_DISPLAY_BASELINE": "34",
            }
        )
        assert_true(
            soak_exit_mismatch.returncode == 0,
            f"device-owned suite should still pass when only soak exit mismatches: {soak_exit_mismatch.stdout}\n{soak_exit_mismatch.stderr}",
        )
        soak_exit_result = json.loads((latest / "result.json").read_text(encoding="utf-8"))
        soak_exit_steps = {step["name"]: step for step in soak_exit_result["steps"]}
        assert_true(soak_exit_result["result"] == "PASS", f"unexpected suite result: {soak_exit_result}")
        assert_true(
            soak_exit_steps["core_soak"]["exit_code"] == 1,
            f"core soak exit should be preserved: {soak_exit_result}",
        )
        assert_true(
            soak_exit_steps["core_soak"]["result"] == "FAIL",
            f"non-zero core exit must override an explicit PASS result: {soak_exit_result}",
        )

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

        # --- Perf CSV parse via --parse-drive-log ---
        csv_artifact_root = temp_root / "csv_artifacts"
        csv_input = temp_root / "perf_boot_test.csv"
        write_perf_csv(csv_input)

        parsed_csv = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(csv_artifact_root),
            },
            "--core",
            "--parse-drive-log",
            str(csv_input),
        )
        # exit 0 or 1 (PASS/PASS_WITH_WARNINGS) depending on missing metrics — not exit 3 (error)
        assert_true(parsed_csv.returncode != 3, f"CSV parse errored: {parsed_csv.stdout}\n{parsed_csv.stderr}")
        csv_latest = csv_artifact_root / "release" / "latest"
        csv_result = json.loads((csv_latest / "result.json").read_text(encoding="utf-8"))
        assert_true([step["name"] for step in csv_result["steps"]] == ["core_soak"], f"CSV steps wrong: {csv_result}")
        csv_scoring = json.loads((csv_latest / "core_soak" / "scoring.json").read_text(encoding="utf-8"))
        csv_metric_names = {m["metric"] for m in csv_scoring["metrics"]}
        assert_true("loop_max_peak_us" in csv_metric_names, "CSV scoring missing loop_max_peak_us")
        assert_true("wifi_p95_us" in csv_metric_names, "CSV scoring missing wifi_p95_us")
        assert_true("dma_fragmentation_pct_p95" in csv_metric_names, "CSV scoring missing dma_fragmentation_pct_p95")
        csv_manifest = json.loads((csv_latest / "core_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(csv_manifest["source_type"] == "perf_csv", f"wrong source type: {csv_manifest}")
        assert_true(csv_manifest["selected_segment"]["session_index"] == 1, f"wrong selected segment: {csv_manifest}")
        assert_true((csv_latest / "core_soak" / "segments.json").exists(), "segments.json missing")
        assert_true((csv_latest / "core_soak" / "csv_scorecard.json").exists(), "csv_scorecard.json missing")
        csv_comparison = (csv_latest / "core_soak" / "comparison.txt").read_text(encoding="utf-8")
        assert_true("Complementary CSV Scorecard" in csv_comparison, "CSV comparison missing scorecard section")
        assert_true("selected_segment:" in csv_comparison, "CSV comparison missing selected segment")

        # --- Legacy perf CSV should surface partial coverage, not fake missing failures ---
        legacy_artifact_root = temp_root / "legacy_csv_artifacts"
        legacy_csv = temp_root / "perf_boot_legacy.csv"
        write_perf_csv(legacy_csv, header_columns=LEGACY_HEADER_COLUMNS)
        parsed_legacy = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(legacy_artifact_root),
            },
            "--core",
            "--parse-drive-log",
            str(legacy_csv),
        )
        assert_true(parsed_legacy.returncode != 3, f"legacy CSV parse errored: {parsed_legacy.stdout}\n{parsed_legacy.stderr}")
        legacy_latest = legacy_artifact_root / "release" / "latest"
        legacy_manifest = json.loads((legacy_latest / "core_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(legacy_manifest["coverage_status"] == "partial_legacy_import", f"wrong legacy coverage: {legacy_manifest}")
        legacy_scoring = json.loads((legacy_latest / "core_soak" / "scoring.json").read_text(encoding="utf-8"))
        legacy_unsupported = {m["metric"] for m in legacy_scoring["metrics"] if m["classification"] == "unsupported"}
        assert_true("perf_drop_delta" in legacy_unsupported, "legacy scoring should mark perf_drop_delta unsupported")
        assert_true("event_drop_delta" in legacy_unsupported, "legacy scoring should mark event_drop_delta unsupported")

        # --- Multi-session perf CSV should support list + auto + explicit segment selection ---
        multi_csv = temp_root / "perf_boot_multi.csv"
        session_one_rows = []
        for i in range(5):
            frac = i / 4
            row = _base_csv_row(int(frac * 120000), header_columns=DRIVE_HEADER_COLUMNS, connected=True, drive_like=False)
            row["rx"] = 200 + i * 40
            row["parseOK"] = 200 + i * 40
            session_one_rows.append(row)
        session_two_rows = []
        for i in range(5):
            frac = i / 4
            row = _base_csv_row(int(frac * 60000), header_columns=DRIVE_HEADER_COLUMNS, connected=True, drive_like=True)
            row["rx"] = 50 + i * 30
            row["parseOK"] = 50 + i * 30
            row["displayUpdates"] = i * 5
            session_two_rows.append(row)
        write_perf_csv(
            multi_csv,
            header_columns=DRIVE_HEADER_COLUMNS,
            sessions=[
                {
                    "meta": "#session_start,seq=1,bootId=1,uptime_ms=120000,token=NODRIVE1,schema=13",
                    "rows": session_one_rows,
                },
                {
                    "meta": "#session_start,seq=2,bootId=1,uptime_ms=60000,token=DRIVE002,schema=13",
                    "rows": session_two_rows,
                },
            ],
        )

        listed_segments = run_test_script(
            common_env,
            "--list-segments",
            "--parse-drive-log",
            str(multi_csv),
        )
        assert_true(listed_segments.returncode == 0, f"list-segments failed: {listed_segments.stdout}\n{listed_segments.stderr}")
        assert_true("DRIVE002" in listed_segments.stdout, f"list-segments missing drive session: {listed_segments.stdout}")

        auto_segment_root = temp_root / "auto_segment_artifacts"
        auto_segment = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(auto_segment_root),
            },
            "--core",
            "--parse-drive-log",
            str(multi_csv),
        )
        assert_true(auto_segment.returncode != 3, f"auto segment parse failed: {auto_segment.stdout}\n{auto_segment.stderr}")
        auto_latest = auto_segment_root / "release" / "latest"
        auto_manifest = json.loads((auto_latest / "core_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(auto_manifest["selected_segment"]["session_index"] == 2, f"auto selector chose wrong segment: {auto_manifest}")
        assert_true(auto_manifest["selected_segment"]["speed_active_rows_supported"] is True, f"extended speed evidence should be detected: {auto_manifest}")

        explicit_segment_root = temp_root / "explicit_segment_artifacts"
        explicit_segment = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(explicit_segment_root),
            },
            "--core",
            "--segment",
            "1",
            "--parse-drive-log",
            str(multi_csv),
        )
        assert_true(explicit_segment.returncode != 3, f"explicit segment parse failed: {explicit_segment.stdout}\n{explicit_segment.stderr}")
        explicit_latest = explicit_segment_root / "release" / "latest"
        explicit_manifest = json.loads((explicit_latest / "core_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(explicit_manifest["selected_segment"]["session_index"] == 1, f"explicit selector chose wrong segment: {explicit_manifest}")

        contract_multi_csv = temp_root / "perf_boot_multi_contract.csv"
        contract_session_one_rows = []
        for i in range(5):
            frac = i / 4
            row = _base_csv_row(int(frac * 120000), header_columns=HEADER_COLUMNS, connected=True, drive_like=False)
            row["rx"] = 200 + i * 40
            row["parseOK"] = 200 + i * 40
            contract_session_one_rows.append(row)
        contract_session_two_rows = []
        for i in range(5):
            frac = i / 4
            row = _base_csv_row(int(frac * 60000), header_columns=HEADER_COLUMNS, connected=True, drive_like=False)
            row["rx"] = 50 + i * 30
            row["parseOK"] = 50 + i * 30
            row["displayUpdates"] = i * 5
            contract_session_two_rows.append(row)
        write_perf_csv(
            contract_multi_csv,
            header_columns=HEADER_COLUMNS,
            sessions=[
                {
                    "meta": "#session_start,seq=1,bootId=1,uptime_ms=120000,token=LONG001,schema=24",
                    "rows": contract_session_one_rows,
                },
                {
                    "meta": "#session_start,seq=2,bootId=1,uptime_ms=60000,token=SHORT002,schema=24",
                    "rows": contract_session_two_rows,
                },
            ],
        )

        contract_listed_segments = run_test_script(
            common_env,
            "--list-segments",
            "--parse-drive-log",
            str(contract_multi_csv),
        )
        assert_true(contract_listed_segments.returncode == 0, f"contract list-segments failed: {contract_listed_segments.stdout}\n{contract_listed_segments.stderr}")
        assert_true("n/a" in contract_listed_segments.stdout, f"contract list-segments should show unsupported speed evidence: {contract_listed_segments.stdout}")

        contract_auto_root = temp_root / "contract_auto_segment_artifacts"
        contract_auto_segment = run_test_script(
            {
                **common_env,
                "HARDWARE_TEST_ARTIFACT_ROOT": str(contract_auto_root),
            },
            "--core",
            "--parse-drive-log",
            str(contract_multi_csv),
        )
        assert_true(contract_auto_segment.returncode != 3, f"contract auto segment parse failed: {contract_auto_segment.stdout}\n{contract_auto_segment.stderr}")
        contract_auto_latest = contract_auto_root / "release" / "latest"
        contract_auto_manifest = json.loads((contract_auto_latest / "core_soak" / "manifest.json").read_text(encoding="utf-8"))
        assert_true(contract_auto_manifest["selected_segment"]["session_index"] == 1, f"contract auto selector should fall back to longest connected: {contract_auto_manifest}")
        assert_true(contract_auto_manifest["selected_segment"]["speed_active_rows_supported"] is False, f"contract auto selector should mark direct speed evidence unsupported: {contract_auto_manifest}")

    print("hardware test script regression tests: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
