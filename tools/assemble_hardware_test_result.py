#!/usr/bin/env python3
"""Assemble the single-entrypoint hardware test rollup artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Assemble hardware test result artifacts")
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--result-json", required=True)
    parser.add_argument("--comparison-txt", required=True)
    parser.add_argument("--run-history-tsv", required=True)
    parser.add_argument("--metric-history-tsv", required=True)
    parser.add_argument("--warning-policy", default="non_blocking")
    parser.add_argument("--board-id", required=True)
    parser.add_argument("--device-port", required=True)
    parser.add_argument("--metrics-url", required=True)
    parser.add_argument("--git-sha", required=True)
    parser.add_argument("--git-ref", required=True)
    parser.add_argument("--previous-run-dir", default="")
    parser.add_argument("--enabled-steps", default="device_tests,core_soak,display_soak")
    parser.add_argument("--device-exit", type=int, default=0)
    parser.add_argument("--core-exit", type=int, default=0)
    parser.add_argument("--display-exit", type=int, default=0)
    parser.add_argument("--strict-soaks", action="store_true")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"[WARN] Invalid JSON in {path}: {exc}", file=sys.stderr)
        return None
    if not isinstance(payload, dict):
        raise RuntimeError(f"Expected object in {path}")
    return payload


def append_tsv_row(path: Path, header: list[str], row: dict[str, object]) -> None:
    exists = path.exists()
    with path.open("a", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=header, delimiter="\t")
        if not exists:
            writer.writeheader()
        writer.writerow({key: row.get(key, "") for key in header})


def compute_step_result(scoring: dict[str, Any] | None, manifest: dict[str, Any] | None, exit_code: int) -> str:
    explicit_result: str | None = None
    if scoring and scoring.get("result"):
        explicit_result = str(scoring["result"])
    elif manifest and manifest.get("result"):
        explicit_result = str(manifest["result"])

    if explicit_result in {"FAIL", "ERROR", "PASS_WITH_WARNINGS", "INCONCLUSIVE", "NO_BASELINE"}:
        return explicit_result

    if exit_code:
        if explicit_result == "PASS":
            return "FAIL"
        return "FAIL"

    if explicit_result:
        return explicit_result
    return "ERROR"


def select_authoritative_steps(enabled_steps: list[str], strict_soaks: bool) -> list[str]:
    if strict_soaks:
        return enabled_steps
    if "device_tests" in enabled_steps:
        return ["device_tests"]
    return enabled_steps


def classify_diagnostic_steps(
    step_rows: list[dict[str, object]],
    authoritative_steps: list[str],
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    authoritative = set(authoritative_steps)
    diagnostic_failures: list[dict[str, str]] = []
    diagnostic_warnings: list[dict[str, str]] = []
    for item in step_rows:
        name = str(item["name"])
        if name in authoritative:
            continue
        result = str(item["result"])
        entry = {"name": name, "result": result}
        if result in {"FAIL", "ERROR"}:
            diagnostic_failures.append(entry)
        elif result in {"PASS_WITH_WARNINGS", "INCONCLUSIVE", "NO_BASELINE"}:
            diagnostic_warnings.append(entry)
    return diagnostic_failures, diagnostic_warnings


def format_step_list(items: list[dict[str, str]]) -> str:
    if not items:
        return "none"
    return ", ".join(f"{item['name']} {item['result']}" for item in items)


def build_rollup_summary(
    suite_result: str,
    authoritative_steps: list[str],
    diagnostic_failures: list[dict[str, str]],
    diagnostic_warnings: list[dict[str, str]],
) -> str:
    authority = ",".join(authoritative_steps) or "n/a"
    parts = [f"Suite {suite_result} ({authority} authoritative)"]
    if diagnostic_failures:
        parts.append(f"diagnostic failures: {format_step_list(diagnostic_failures)}")
    if diagnostic_warnings:
        parts.append(f"diagnostic warnings: {format_step_list(diagnostic_warnings)}")
    return "; ".join(parts)


def overall_result(step_rows: list[dict[str, object]], authoritative_steps: list[str]) -> str:
    authoritative = set(authoritative_steps)
    results = [str(item["result"]) for item in step_rows if str(item["name"]) in authoritative]
    if not results:
        results = [str(item["result"]) for item in step_rows]
    if any(result in {"FAIL", "ERROR"} for result in results):
        return "FAIL"
    if any(result in {"PASS_WITH_WARNINGS", "INCONCLUSIVE"} for result in results):
        return "PASS_WITH_WARNINGS"
    if results and all(result == "NO_BASELINE" for result in results):
        return "NO_BASELINE"
    if any(result == "NO_BASELINE" for result in results):
        return "PASS_WITH_WARNINGS"
    return "PASS"


def main() -> int:
    args = parse_args()
    run_dir = Path(args.run_dir)
    result_json = Path(args.result_json)
    comparison_txt = Path(args.comparison_txt)
    run_history_tsv = Path(args.run_history_tsv)
    metric_history_tsv = Path(args.metric_history_tsv)

    enabled_steps = [item.strip() for item in args.enabled_steps.split(",") if item.strip()]
    if not enabled_steps:
        raise RuntimeError("At least one enabled step is required")

    step_exit_codes = {
        "device_tests": args.device_exit,
        "core_soak": args.core_exit,
        "display_soak": args.display_exit,
    }

    steps: list[dict[str, object]] = []
    for step_name in enabled_steps:
        if step_name not in step_exit_codes:
            raise RuntimeError(f"Unsupported step name '{step_name}'")
        step_dir = run_dir / step_name
        manifest = load_json(step_dir / "manifest.json")
        scoring = load_json(step_dir / "scoring.json")
        steps.append(
            {
                "name": step_name,
                "exit_code": step_exit_codes[step_name],
                "result": compute_step_result(scoring, manifest, step_exit_codes[step_name]),
                "comparison_kind": str((scoring or {}).get("comparison_kind", "no_scoring")),
                "manifest_path": str(step_dir / "manifest.json") if (step_dir / "manifest.json").exists() else "",
                "scoring_path": str(step_dir / "scoring.json") if (step_dir / "scoring.json").exists() else "",
                "comparison_text_path": str(step_dir / "comparison.txt") if (step_dir / "comparison.txt").exists() else "",
                "comparison_tsv_path": str(step_dir / "comparison.tsv") if (step_dir / "comparison.tsv").exists() else "",
            }
        )

    authoritative_steps = select_authoritative_steps(enabled_steps, args.strict_soaks)
    suite_result = overall_result(steps, authoritative_steps)
    diagnostic_failures, diagnostic_warnings = classify_diagnostic_steps(steps, authoritative_steps)
    rollup_summary = build_rollup_summary(suite_result, authoritative_steps, diagnostic_failures, diagnostic_warnings)
    timestamp_utc = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    payload = {
        "schema_version": 1,
        "test_name": "hardware_test",
        "timestamp_utc": timestamp_utc,
        "board_id": args.board_id,
        "warning_policy": args.warning_policy,
        "device_port": args.device_port,
        "metrics_url": args.metrics_url,
        "git_sha": args.git_sha,
        "git_ref": args.git_ref,
        "run_dir": str(run_dir),
        "previous_run_dir": args.previous_run_dir,
        "enabled_steps": enabled_steps,
        "strict_soaks": args.strict_soaks,
        "authoritative_steps": authoritative_steps,
        "result": suite_result,
        "diagnostic_failures": diagnostic_failures,
        "diagnostic_warnings": diagnostic_warnings,
        "rollup_summary": rollup_summary,
        "steps": steps,
    }
    result_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    lines = [
        f"hardware_test: {args.board_id}",
        f"result: {suite_result}",
        f"warning_policy: {args.warning_policy}",
        f"git: {args.git_sha} ({args.git_ref})",
        f"run_dir: {run_dir}",
        f"previous_run_dir: {args.previous_run_dir or 'n/a'}",
        f"strict_soaks: {'yes' if args.strict_soaks else 'no'}",
        f"authoritative_steps: {','.join(authoritative_steps) or 'n/a'}",
        f"diagnostic_failures: {format_step_list(diagnostic_failures)}",
        f"diagnostic_warnings: {format_step_list(diagnostic_warnings)}",
        f"rollup_summary: {rollup_summary}",
        "",
        "STEP            RESULT              COMPARE             EXIT  READABLE_METRICS",
        "--------------- ------------------- ------------------- ----- ----------------------------------------------",
    ]
    for step in steps:
        lines.append(
            f"{step['name']:<15} "
            f"{step['result']:<19} "
            f"{step['comparison_kind']:<19} "
            f"{step['exit_code']:<5} "
            f"{step['comparison_text_path'] or 'n/a'}"
        )
    comparison_txt.write_text("\n".join(lines) + "\n", encoding="utf-8")

    append_tsv_row(
        run_history_tsv,
        [
            "timestamp_utc",
            "board_id",
            "git_sha",
            "git_ref",
            "warning_policy",
            "result",
            "run_dir",
            "previous_run_dir",
            "enabled_steps",
            "device_result",
            "core_result",
            "display_result",
        ],
        {
            "timestamp_utc": timestamp_utc,
            "board_id": args.board_id,
            "git_sha": args.git_sha,
            "git_ref": args.git_ref,
            "warning_policy": args.warning_policy,
            "result": suite_result,
            "run_dir": str(run_dir),
            "previous_run_dir": args.previous_run_dir,
            "enabled_steps": ",".join(enabled_steps),
            "device_result": next((step["result"] for step in steps if step["name"] == "device_tests"), ""),
            "core_result": next((step["result"] for step in steps if step["name"] == "core_soak"), ""),
            "display_result": next((step["result"] for step in steps if step["name"] == "display_soak"), ""),
        },
    )

    for step in steps:
        scoring_path_text = str(step["scoring_path"] or "")
        if not scoring_path_text:
            continue
        scoring_path = Path(scoring_path_text)
        if not scoring_path.is_file():
            continue
        scoring = load_json(scoring_path) or {}
        for metric in scoring.get("metrics", []):
            append_tsv_row(
                metric_history_tsv,
                [
                    "timestamp_utc",
                    "board_id",
                    "git_sha",
                    "git_ref",
                    "suite_step",
                    "suite_or_profile",
                    "metric",
                    "unit",
                    "result",
                    "comparison_kind",
                    "score_status",
                    "classification",
                    "current_value",
                    "baseline_value",
                    "delta_abs",
                    "delta_pct",
                ],
                {
                    "timestamp_utc": timestamp_utc,
                    "board_id": args.board_id,
                    "git_sha": args.git_sha,
                    "git_ref": args.git_ref,
                    "suite_step": step["name"],
                    "suite_or_profile": metric.get("suite_or_profile", ""),
                    "metric": metric.get("metric", ""),
                    "unit": metric.get("unit", ""),
                    "result": step["result"],
                    "comparison_kind": step["comparison_kind"],
                    "score_status": metric.get("score_status", ""),
                    "classification": metric.get("classification", ""),
                    "current_value": metric.get("current_value", ""),
                    "baseline_value": metric.get("baseline_value", ""),
                    "delta_abs": metric.get("delta_abs", ""),
                    "delta_pct": metric.get("delta_pct", ""),
                },
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
