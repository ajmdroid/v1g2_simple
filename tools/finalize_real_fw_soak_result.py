#!/usr/bin/env python3
"""Resolve the final result for a real firmware soak run."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


VALID_RESULTS = {"PASS", "PASS_WITH_WARNINGS", "FAIL", "INCONCLUSIVE", "NO_BASELINE", "ERROR"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Finalize the result for run_real_fw_soak.sh")
    parser.add_argument("--manifest", required=True, help="Path to manifest.json")
    parser.add_argument("--runtime-result", required=True, help="Pre-trend soak result")
    parser.add_argument("--trend-scorer-exit", type=int, required=True, help="Exit code from score_hardware_run.py")
    parser.add_argument("--trend-scoring-json", default="", help="Path to scoring.json")
    parser.add_argument("--trend-skipped", action="store_true", help="Skip applying trend scoring")
    parser.add_argument("--allow-inconclusive", action="store_true", help="Allow INCONCLUSIVE to exit zero")
    parser.add_argument(
        "--trend-error-detail",
        default="",
        help="Optional scorer failure detail to persist when scoring errors",
    )
    parser.add_argument("--summary-body", default="", help="Path to summary body markdown")
    parser.add_argument("--summary-output", default="", help="Path to write the final summary markdown")
    parser.add_argument("--trend-summary", default="", help="Path to trend summary markdown")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"Expected object in {path}")
    return payload


def resolve_result(
    runtime_result: str,
    trend_skipped: bool,
    trend_scorer_exit: int,
    scoring: dict[str, Any] | None,
    allow_inconclusive: bool,
    trend_error_detail: str,
) -> dict[str, Any]:
    if runtime_result not in VALID_RESULTS:
        raise RuntimeError(f"Unsupported runtime result '{runtime_result}'")

    trend_status = "skipped" if trend_skipped else "applied"
    trend_result = None
    trend_error = ""

    if trend_skipped:
        final_result = runtime_result
    else:
        scoring_result = None if scoring is None else scoring.get("result")
        if trend_scorer_exit <= 2 and isinstance(scoring_result, str) and scoring_result in VALID_RESULTS:
            trend_result = scoring_result
            final_result = trend_result
        else:
            trend_status = "error"
            detail = trend_error_detail.strip()
            if not detail:
                if scoring is None:
                    detail = f"Trend scorer exited {trend_scorer_exit} without a readable scoring.json payload."
                else:
                    detail = f"Trend scorer exited {trend_scorer_exit} with an invalid or unsupported result."
            trend_error = detail
            final_result = "FAIL" if runtime_result == "FAIL" else "ERROR"

    exit_code = 0
    if final_result in {"FAIL", "PASS_WITH_WARNINGS", "ERROR"}:
        exit_code = 1
    elif final_result == "INCONCLUSIVE" and not allow_inconclusive:
        exit_code = 2

    return {
        "runtime_result": runtime_result,
        "trend_status": trend_status,
        "trend_result": trend_result,
        "result": final_result,
        "exit_code": exit_code,
        "trend_error": trend_error,
    }


def render_summary(
    resolved: dict[str, Any],
    summary_body: str,
    trend_summary: str,
) -> str:
    lines: list[str] = [
        "# Real Firmware Soak Summary",
        "",
        f"- Result: **{resolved['result']}**",
    ]
    if resolved["result"] != resolved["runtime_result"]:
        lines.append(f"- Runtime result: **{resolved['runtime_result']}**")
    if resolved["trend_status"] == "applied" and resolved["trend_result"]:
        lines.append(f"- Trend result: **{resolved['trend_result']}**")
    lines.append(f"- Trend status: `{resolved['trend_status']}`")
    if resolved["trend_status"] == "error" and resolved["trend_error"]:
        lines.append(f"- Trend error: {resolved['trend_error']}")
    lines.append("")

    summary_body = summary_body.rstrip()
    if summary_body:
        lines.append(summary_body)
        lines.append("")

    lines.extend(["## Trend Comparison", ""])
    if resolved["trend_status"] == "applied":
        trend_summary = trend_summary.rstrip()
        if trend_summary:
            lines.append(trend_summary)
        else:
            lines.append("- Result: **ERROR**")
            lines.append("- Trend scoring applied, but no human-readable summary was generated.")
    elif resolved["trend_status"] == "skipped":
        lines.append("- Result: **SKIPPED**")
        lines.append("- Structured trend scoring skipped because no trend metrics were emitted.")
    else:
        lines.append("- Result: **ERROR**")
        detail = resolved["trend_error"] or "Structured trend scoring failed before a comparison summary could be generated."
        lines.append(f"- {detail}")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    args = parse_args()
    manifest_path = Path(args.manifest)
    manifest = load_json(manifest_path)

    scoring = None
    scoring_path = Path(args.trend_scoring_json) if args.trend_scoring_json else None
    if not args.trend_skipped and scoring_path and scoring_path.exists():
        try:
            scoring = load_json(scoring_path)
        except json.JSONDecodeError as exc:
            args.trend_error_detail = f"Invalid scoring.json payload: {exc}"

    resolved = resolve_result(
        runtime_result=args.runtime_result,
        trend_skipped=args.trend_skipped,
        trend_scorer_exit=args.trend_scorer_exit,
        scoring=scoring,
        allow_inconclusive=args.allow_inconclusive,
        trend_error_detail=args.trend_error_detail,
    )

    manifest["runtime_result"] = resolved["runtime_result"]
    manifest["base_result"] = resolved["runtime_result"]
    manifest["result"] = resolved["result"]
    manifest["trend_status"] = resolved["trend_status"]
    if resolved["trend_result"]:
        manifest["trend_result"] = resolved["trend_result"]
    else:
        manifest.pop("trend_result", None)
    if resolved["trend_error"]:
        manifest["trend_error"] = resolved["trend_error"]
    else:
        manifest.pop("trend_error", None)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    if args.summary_output:
        summary_body = ""
        if args.summary_body:
            summary_body = Path(args.summary_body).read_text(encoding="utf-8")
        trend_summary = ""
        if args.trend_summary and Path(args.trend_summary).exists():
            trend_summary = Path(args.trend_summary).read_text(encoding="utf-8")
        Path(args.summary_output).write_text(
            render_summary(resolved, summary_body, trend_summary),
            encoding="utf-8",
        )

    print(json.dumps(resolved, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
