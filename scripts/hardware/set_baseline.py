#!/usr/bin/env python3
"""Promote selected hardware test runs into a local baseline override window."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ARTIFACT_ROOT = ROOT / ".artifacts" / "hardware" / "test"
STEP_NAMES = ("device_tests", "core_soak", "display_soak")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--board-id",
        default="release",
        help="Board id under .artifacts/hardware/test (default: release)",
    )
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=DEFAULT_ARTIFACT_ROOT,
        help="Hardware test artifact root (default: .artifacts/hardware/test)",
    )
    parser.add_argument(
        "--run-dir",
        action="append",
        required=True,
        help="Run directory to include in the promoted baseline window; repeat up to 3 times",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Optional override path for baseline_manifest_overrides.json",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def normalize_run_dirs(run_dirs: list[str]) -> list[Path]:
    normalized = []
    for raw in run_dirs:
        candidate = Path(raw).resolve()
        if not candidate.is_dir():
            raise SystemExit(f"Run directory not found: {candidate}")
        normalized.append(candidate)
    unique = sorted(set(normalized), reverse=True)
    if len(unique) > 3:
        raise SystemExit("At most 3 run directories may be promoted at once.")
    return unique


def promote_manifest(source_manifest_path: Path, baseline_dir: Path) -> Path:
    payload = load_json(source_manifest_path)
    result = str(payload.get("result") or "")
    runtime_result = str(payload.get("runtime_result") or payload.get("base_result") or "")
    base_result = str(payload.get("base_result") or "")
    if runtime_result != "PASS" or base_result != "PASS":
        raise SystemExit(
            f"Refusing to promote non-runtime-clean manifest: {source_manifest_path} "
            f"(result={result} runtime_result={runtime_result} base_result={base_result})"
        )

    metrics_path = source_manifest_path.parent / str(payload.get("metrics_file") or "")
    scoring_path = source_manifest_path.parent / str(payload.get("scoring_file") or "")
    payload["result"] = "PASS_WITH_WARNINGS"
    payload["baseline_promoted"] = True
    payload["baseline_promoted_at_utc"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    payload["baseline_promoted_from_result"] = result
    payload["baseline_promoted_from_manifest"] = str(source_manifest_path.resolve())
    if metrics_path.is_file():
        payload["metrics_file"] = str(metrics_path.resolve())
    if scoring_path.is_file():
        payload["scoring_file"] = str(scoring_path.resolve())

    promoted_path = baseline_dir / f"{source_manifest_path.parent.name}_manifest.json"
    promoted_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return promoted_path


def main() -> int:
    args = parse_args()
    board_root = args.artifact_root.resolve() / args.board_id
    run_dirs = normalize_run_dirs(args.run_dir)
    output_path = (args.output.resolve() if args.output is not None else board_root / "baseline_manifest_overrides.json")
    baseline_dir = board_root / "baselines" / datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    baseline_dir.mkdir(parents=True, exist_ok=True)

    steps: dict[str, list[str]] = {step_name: [] for step_name in STEP_NAMES}
    for index, run_dir in enumerate(run_dirs, start=1):
        result_path = run_dir / "result.json"
        if not result_path.is_file():
            raise SystemExit(f"Missing result.json in run dir: {run_dir}")
        result_payload = load_json(result_path)
        step_payloads = {str(item.get("name")): item for item in result_payload.get("steps") or [] if isinstance(item, dict)}

        for step_name in STEP_NAMES:
            step_payload = step_payloads.get(step_name)
            if not isinstance(step_payload, dict):
                raise SystemExit(f"Run dir {run_dir} missing step '{step_name}' in result.json")
            source_manifest_path = Path(str(step_payload.get("manifest_path") or "")).resolve()
            if not source_manifest_path.is_file():
                raise SystemExit(f"Manifest missing for {step_name} in {run_dir}: {source_manifest_path}")
            promoted_dir = baseline_dir / f"run{index}_{run_dir.name}"
            promoted_dir.mkdir(parents=True, exist_ok=True)
            promoted_path = promote_manifest(source_manifest_path, promoted_dir)
            steps[step_name].append(str(promoted_path))

    payload = {
        "schema_version": 1,
        "board_id": args.board_id,
        "updated_at_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "previous_run_dir": str(run_dirs[0]),
        "steps": steps,
        "source_run_dirs": [str(run_dir) for run_dir in run_dirs],
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    print(f"Baseline override written: {output_path}")
    print(f"Promoted baseline manifests: {baseline_dir}")
    for step_name, manifests in steps.items():
        print(f"{step_name}: {len(manifests)} manifest(s)")
        for manifest in manifests:
            print(f"  {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
