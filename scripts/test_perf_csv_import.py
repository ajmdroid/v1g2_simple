#!/usr/bin/env python3
"""Deterministic regression tests for perf CSV import into hardware scoring."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import import_perf_csv  # type: ignore  # noqa: E402

HEADER_COLUMNS = [
    line.strip()
    for line in (ROOT / "test" / "contracts" / "perf_csv_column_contract.txt").read_text(encoding="utf-8").splitlines()
    if line.strip() and not line.startswith("#")
]
LEGACY_HEADER_COLUMNS = HEADER_COLUMNS[:-4]


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def base_row(millis: int, *, connected: bool, header_columns: list[str]) -> dict[str, int]:
    row = {column: 0 for column in header_columns}
    row.update(
        {
            "millis": millis,
            "timeValid": 1,
            "timeSource": 1,
            "qDrop": 0,
            "parseFail": 0,
            "oversizeDrops": 0,
            "bleMutexTimeout": 0,
            "loopMax_us": 80000,
            "bleDrainMax_us": 4000,
            "bleProcessMax_us": 40000,
            "dispPipeMax_us": 30000,
            "flushMax_us": 20000,
            "sdMax_us": 10000,
            "fsMax_us": 10000,
            "queueHighWater": 5,
            "freeDma": 30000,
            "largestDma": 18000,
            "dmaLargestMin": 9000,
            "dmaFreeMin": 12000,
            "wifiConnectDeferred": 0,
            "wifiMax_us": 600,
            "displayUpdates": 0,
            "displaySkips": 0,
            "cmdPaceNotYet": 0,
            "gpsObsDrops": 0,
            "audioPlayBusy": 0,
            "reconn": 0,
            "disc": 0,
            "rx": 0,
            "parseOK": 0,
            "gpsHasFix": 0,
            "gpsLocationValid": 0,
            "gpsSpeedMph_x10": 0,
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
        row["rx"] = 100
        row["parseOK"] = 100
    return row


def make_session(
    *,
    seq: int,
    token: str,
    schema: int,
    header_columns: list[str],
    duration_ms: int,
    connected: bool,
    drive_like: bool = False,
    row_count: int = 5,
    end_overrides: dict[str, int] | None = None,
) -> dict[str, object]:
    rows = []
    for index in range(row_count):
        frac = index / max(row_count - 1, 1)
        row = base_row(int(frac * duration_ms), connected=connected, header_columns=header_columns)
        if connected:
            row["rx"] = 100 + 50 * index
            row["parseOK"] = 100 + 50 * index
            row["displayUpdates"] = 8 * index
        if drive_like:
            row["gpsHasFix"] = 1
            row["gpsLocationValid"] = 1
            row["gpsSpeedMph_x10"] = 250 + 10 * index
        rows.append(row)
    if end_overrides:
        rows[-1].update(end_overrides)
    return {
        "meta": f"#session_start,seq={seq},bootId={seq},uptime_ms={duration_ms},token={token},schema={schema}",
        "rows": rows,
    }


def write_capture(
    path: Path,
    *,
    header_columns: list[str],
    sessions: list[dict[str, object]],
    leading_rows: list[dict[str, int]] | None = None,
) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=header_columns)
        if leading_rows:
            for row in leading_rows:
                writer.writerow({key: row.get(key, 0) for key in header_columns})
        for session in sessions:
            writer.writeheader()
            handle.write(f"{session['meta']}\n")
            for row in session["rows"]:
                writer.writerow({key: row.get(key, 0) for key in header_columns})


def run_import(
    csv_path: Path,
    out_dir: Path,
    *extra_args: str,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(csv_path),
            "--out-dir",
            str(out_dir),
            "--profile",
            "drive_wifi_ap",
            "--board-id",
            "test",
            "--git-sha",
            "abc1234",
            "--git-ref",
            "test-branch",
            *extra_args,
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_dma_fragmentation_uses_current_values() -> None:
    rows: list[dict[str, int]] = []
    for index, free_dma in enumerate([20000, 18000, 16000, 14000, 12000]):
        row = base_row(index * 1000, connected=True, header_columns=LEGACY_HEADER_COLUMNS)
        row["freeDma"] = free_dma
        row["largestDma"] = 10000
        row["dmaFreeMin"] = 50000
        row["dmaLargestMin"] = 49000
        rows.append(row)
    metrics, _peaks, unsupported = import_perf_csv.extract_metrics(rows, 12)
    expected = import_perf_csv._percentile([(1.0 - (10000.0 / free_dma)) * 100.0 for free_dma in [20000, 18000, 16000, 14000, 12000]], 95)
    assert_true(abs(metrics["dma_fragmentation_pct_p95"][0] - float(expected)) < 0.01, "fragmentation must use current freeDma/largestDma values")
    assert_true("perf_drop_delta" in unsupported and "event_drop_delta" in unsupported, "legacy schema should mark drop deltas unsupported")


def test_legacy_import_reports_partial_coverage(tmpdir: Path) -> None:
    csv_path = tmpdir / "legacy.csv"
    out_dir = tmpdir / "legacy_out"
    write_capture(
        csv_path,
        header_columns=LEGACY_HEADER_COLUMNS,
        sessions=[
            make_session(
                seq=1,
                token="LEGACY01",
                schema=12,
                header_columns=LEGACY_HEADER_COLUMNS,
                duration_ms=60000,
                connected=True,
                drive_like=True,
            )
        ],
    )
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"legacy import error: {result.stderr}")
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    scoring = json.loads((out_dir / "scoring.json").read_text(encoding="utf-8"))
    assert_true(manifest["source_type"] == "perf_csv", "legacy import should set source_type")
    assert_true(manifest["source_schema"] == 12, f"wrong source schema: {manifest}")
    assert_true(manifest["coverage_status"] == "partial_legacy_import", f"wrong coverage status: {manifest}")
    assert_true(set(["perf_drop_delta", "event_drop_delta", "samples_to_stable", "time_to_stable_ms"]).issubset(set(manifest["unsupported_metrics"])), f"unsupported metrics missing: {manifest}")
    unsupported_metrics = {metric["metric"] for metric in scoring["metrics"] if metric["classification"] == "unsupported"}
    assert_true("perf_drop_delta" in unsupported_metrics, "legacy scoring should mark perf_drop_delta unsupported")
    assert_true("event_drop_delta" in unsupported_metrics, "legacy scoring should mark event_drop_delta unsupported")
    assert_true(scoring["summary"]["hard_failures"] == 0, f"legacy unsupported fields must not hard-fail: {scoring}")
    comparison = (out_dir / "comparison.txt").read_text(encoding="utf-8")
    assert_true("coverage_status: partial_legacy_import" in comparison, "comparison should show coverage status")
    assert_true("UNSUPPORTED" in comparison, "comparison should show unsupported metrics")


def test_schema13_import_supports_drop_metrics(tmpdir: Path) -> None:
    csv_path = tmpdir / "schema13.csv"
    out_dir = tmpdir / "schema13_out"
    write_capture(
        csv_path,
        header_columns=HEADER_COLUMNS,
        sessions=[
            make_session(
                seq=1,
                token="SCHEMA013",
                schema=13,
                header_columns=HEADER_COLUMNS,
                duration_ms=60000,
                connected=True,
                drive_like=True,
                end_overrides={"perfDrop": 0, "eventBusDrops": 0},
            )
        ],
    )
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"schema13 import error: {result.stderr}")
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    scoring = json.loads((out_dir / "scoring.json").read_text(encoding="utf-8"))
    metric_names = {metric["metric"] for metric in scoring["metrics"] if metric["classification"] != "unsupported"}
    assert_true(manifest["coverage_status"] == "full_runtime_gates", f"wrong coverage status: {manifest}")
    assert_true("perf_drop_delta" in metric_names, "schema13 import should emit perf_drop_delta")
    assert_true("event_drop_delta" in metric_names, "schema13 import should emit event_drop_delta")
    assert_true("perf_drop_delta" not in set(manifest["unsupported_metrics"]), "schema13 perf_drop_delta should not be unsupported")
    diagnostics = json.loads((out_dir / "import_diagnostics.json").read_text(encoding="utf-8"))
    assert_true("loop_max_peak_us" in diagnostics["peaks"], "peak diagnostics missing loop_max_peak_us")


def test_segment_selection_and_listing(tmpdir: Path) -> None:
    csv_path = tmpdir / "multi.csv"
    out_dir = tmpdir / "multi_out"
    session_1 = make_session(
        seq=1,
        token="NODRIVE1",
        schema=13,
        header_columns=HEADER_COLUMNS,
        duration_ms=120000,
        connected=True,
        drive_like=False,
    )
    session_2 = make_session(
        seq=2,
        token="DRIVE002",
        schema=13,
        header_columns=HEADER_COLUMNS,
        duration_ms=60000,
        connected=True,
        drive_like=True,
    )
    write_capture(csv_path, header_columns=HEADER_COLUMNS, sessions=[session_1, session_2])

    listed = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "import_perf_csv.py"),
            "--input",
            str(csv_path),
            "--list-segments",
            "--segment",
            "auto",
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert_true(listed.returncode == 0, f"list-segments failed: {listed.stderr}")
    assert_true("DRIVE002" in listed.stdout, f"list-segments missing drive session: {listed.stdout}")

    auto_result = run_import(csv_path, out_dir)
    assert_true(auto_result.returncode != 3, f"auto import failed: {auto_result.stderr}")
    auto_manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert_true(auto_manifest["selected_segment"]["session_index"] == 2, f"auto selector chose wrong segment: {auto_manifest}")

    explicit_out = tmpdir / "explicit_out"
    explicit_result = run_import(csv_path, explicit_out, "--segment", "1")
    assert_true(explicit_result.returncode != 3, f"explicit import failed: {explicit_result.stderr}")
    explicit_manifest = json.loads((explicit_out / "manifest.json").read_text(encoding="utf-8"))
    assert_true(explicit_manifest["selected_segment"]["session_index"] == 1, f"explicit selector chose wrong segment: {explicit_manifest}")


def test_leading_rows_form_implicit_segment(tmpdir: Path) -> None:
    csv_path = tmpdir / "leading_rows.csv"
    out_dir = tmpdir / "leading_rows_out"
    leading_row = base_row(5000, connected=True, header_columns=HEADER_COLUMNS)
    leading_row["rx"] = 150
    leading_row["parseOK"] = 150
    leading_row["gpsHasFix"] = 1
    leading_row["gpsLocationValid"] = 1
    leading_row["gpsSpeedMph_x10"] = 300
    later_session = make_session(
        seq=2,
        token="LATER002",
        schema=13,
        header_columns=HEADER_COLUMNS,
        duration_ms=10000,
        connected=True,
        drive_like=False,
    )
    write_capture(csv_path, header_columns=HEADER_COLUMNS, sessions=[later_session], leading_rows=[leading_row])
    result = run_import(csv_path, out_dir)
    assert_true(result.returncode != 3, f"leading-row import failed: {result.stderr}")
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    assert_true(manifest["selected_segment"]["session_index"] == 1, f"leading rows should become segment 1: {manifest}")


def main() -> int:
    test_dma_fragmentation_uses_current_values()
    print("[perf-csv-import] metric derivation tests passed")

    with tempfile.TemporaryDirectory(prefix="perf_csv_import_") as tmp:
        tmpdir = Path(tmp)
        test_legacy_import_reports_partial_coverage(tmpdir)
        test_schema13_import_supports_drop_metrics(tmpdir)
        test_segment_selection_and_listing(tmpdir)
        test_leading_rows_form_implicit_segment(tmpdir)

    print("[perf-csv-import] integration tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[perf-csv-import] FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
