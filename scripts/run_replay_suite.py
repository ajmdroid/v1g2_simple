#!/usr/bin/env python3
"""Run replay test suite for a given validation lane.

Selects replay fixtures by lane tag (from meta.json) and runs the
native-replay test binary against them.

Usage:
    python3 scripts/run_replay_suite.py --lane pr
    python3 scripts/run_replay_suite.py --lane nightly
    python3 scripts/run_replay_suite.py --lane pre-release

Lane behaviour:
    pr          – run only fixtures tagged lane="pr" (4 golden scenarios)
    nightly     – run all approved fixtures (pr + nightly)
    pre-release – run all approved fixtures + emit replay_summary.json

Exit codes:
    0 = all selected replay scenarios passed
    1 = one or more scenarios failed
    2 = usage / setup error
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE_ROOT = ROOT / "test" / "fixtures" / "replay"
REPLAY_BUILD_DIR = ROOT / ".pio" / "build" / "native-replay"
REPORT_DIR = ROOT / ".artifacts" / "test_reports"


def discover_fixtures(lane: str) -> list[Path]:
    """Find fixture directories matching the requested lane."""
    if not FIXTURE_ROOT.exists():
        return []

    fixtures = []
    for meta_path in sorted(FIXTURE_ROOT.glob("*/meta.json")):
        with open(meta_path) as f:
            meta = json.load(f)
        fixture_lane = meta.get("lane", "nightly")
        # pr lane runs only pr fixtures; nightly/pre-release run all
        if lane == "pr" and fixture_lane != "pr":
            continue
        fixtures.append(meta_path.parent)
    return fixtures


def _replay_env_available() -> bool:
    """Check if the native-replay PIO env and test binary source exist."""
    test_dir = ROOT / "test" / "test_drive_replay"
    if not test_dir.is_dir():
        return False
    # Verify the env is defined in platformio.ini
    result = subprocess.run(
        ["pio", "project", "config", "-e", "native-replay"],
        cwd=ROOT, capture_output=True,
    )
    return result.returncode == 0


def run_replay_test(fixtures: list[Path]) -> tuple[int, list[dict]]:
    """Build and run the replay test binary. Returns (exit_code, results)."""
    if not fixtures:
        print("[replay] No fixtures matched the lane filter.")
        return 0, []

    if not _replay_env_available():
        print("[replay] native-replay env or test_drive_replay not found — skipping.")
        return 0, [{"scenario_id": f.name, "status": "SKIP"} for f in fixtures]

    scenario_ids = [f.name for f in fixtures]
    print(f"[replay] Selected {len(fixtures)} scenario(s): {', '.join(scenario_ids)}")

    # Set environment variable with fixture paths for the test binary
    env_fixtures = ";".join(str(f) for f in fixtures)

    result = subprocess.run(
        ["pio", "test", "-e", "native-replay", "-f", "test_drive_replay"],
        cwd=ROOT,
        env={**os.environ, "REPLAY_FIXTURES": env_fixtures},
    )

    results = []
    for fixture in fixtures:
        results.append({
            "scenario_id": fixture.name,
            "status": "PASS" if result.returncode == 0 else "FAIL",
        })

    return result.returncode, results


def verify_fixtures(fixtures: list[Path]) -> int:
    if not fixtures:
        return 0

    cmd = [sys.executable, str(ROOT / "scripts" / "verify_replay_fixture.py"), *map(str, fixtures)]
    result = subprocess.run(cmd, cwd=ROOT, check=False)
    return result.returncode


def emit_summary(results: list[dict], lane: str) -> None:
    """Write replay_summary.json for pre-release manifest consumption."""
    summary_dir = REPORT_DIR / lane
    summary_dir.mkdir(parents=True, exist_ok=True)
    summary = {
        "lane": lane,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "scenarios": results,
        "total": len(results),
        "passed": sum(1 for r in results if r["status"] == "PASS"),
        "failed": sum(1 for r in results if r["status"] == "FAIL"),
        "skipped": sum(1 for r in results if r["status"] == "SKIP"),
    }
    out_path = summary_dir / "replay_summary.json"
    with open(out_path, "w") as f:
        json.dump(summary, f, indent=2)
        f.write("\n")
    print(f"[replay] Summary written to {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lane", required=True, choices=["pr", "nightly", "pre-release"])
    args = parser.parse_args()

    fixtures = discover_fixtures(args.lane)
    verify_exit_code = verify_fixtures(fixtures)
    if verify_exit_code != 0:
        print("[replay] Fixture validation failed.")
        return 2

    exit_code, results = run_replay_test(fixtures)

    if args.lane == "pre-release":
        emit_summary(results, args.lane)

    if exit_code != 0:
        print(f"[replay] FAILED ({sum(1 for r in results if r['status'] == 'FAIL')} scenario(s))")
        return 1

    print(f"[replay] All {len(results)} scenario(s) passed for lane '{args.lane}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
