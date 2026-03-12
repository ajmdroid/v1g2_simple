#!/usr/bin/env python3
"""Score structured hardware run manifests against a canonical metric catalog."""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG_PATH = ROOT / "tools" / "hardware_metric_catalog.json"


@dataclass(frozen=True)
class MetricPolicy:
    metric: str
    run_kind: str
    selector: dict[str, Any]
    unit: str
    aggregation: str
    direction: str
    score_level: str
    required: bool
    absolute_min: Optional[float]
    absolute_max: Optional[float]
    regress_abs: Optional[float]
    regress_pct: Optional[float]


@dataclass
class MetricAggregate:
    metric: str
    run_kind: str
    suite_or_profile: str
    value: float
    unit: str
    sample_count: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Score a hardware run manifest")
    parser.add_argument("manifest", help="Path to manifest.json")
    parser.add_argument(
        "--catalog",
        default=str(DEFAULT_CATALOG_PATH),
        help=f"Path to hardware metric catalog (default: {DEFAULT_CATALOG_PATH})",
    )
    parser.add_argument(
        "--compare-to",
        default="",
        help="Optional baseline manifest for run-to-run or commit-to-commit comparison",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of human-readable text")
    return parser.parse_args()


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _resolve_manifest_ref(manifest_path: Path, ref: str) -> Path:
    path = Path(ref)
    if path.is_absolute():
        return path
    return (manifest_path.parent / path).resolve()


def _coerce_optional_float(payload: dict[str, Any], key: str) -> Optional[float]:
    raw = payload.get(key)
    if raw is None:
        return None
    return float(raw)


def load_catalog(path: Path) -> list[MetricPolicy]:
    payload = _load_json(path)
    if not isinstance(payload, dict) or not isinstance(payload.get("metrics"), list):
        raise RuntimeError(f"Invalid catalog format in {path}")

    policies: list[MetricPolicy] = []
    seen: set[tuple[str, str, str]] = set()
    for idx, entry in enumerate(payload["metrics"]):
        if not isinstance(entry, dict):
            raise RuntimeError(f"Invalid catalog entry at index {idx}: expected object")
        selector = entry.get("selector") or {}
        if not isinstance(selector, dict):
            raise RuntimeError(f"Invalid selector in catalog entry {idx}")
        policy = MetricPolicy(
            metric=str(entry["metric"]),
            run_kind=str(entry["run_kind"]),
            selector=selector,
            unit=str(entry["unit"]),
            aggregation=str(entry["aggregation"]),
            direction=str(entry["direction"]),
            score_level=str(entry["score_level"]),
            required=bool(entry["required"]),
            absolute_min=_coerce_optional_float(entry, "absolute_min"),
            absolute_max=_coerce_optional_float(entry, "absolute_max"),
            regress_abs=_coerce_optional_float(entry, "regress_abs"),
            regress_pct=_coerce_optional_float(entry, "regress_pct"),
        )
        selector_key = json.dumps(selector, sort_keys=True)
        key = (policy.run_kind, policy.metric, selector_key)
        if key in seen:
            raise RuntimeError(f"Duplicate policy in catalog for {policy.run_kind}/{policy.metric} selector={selector_key}")
        seen.add(key)
        policies.append(policy)
    return policies


def load_manifest(path: Path) -> dict[str, Any]:
    payload = _load_json(path)
    required = [
        "schema_version",
        "run_id",
        "timestamp_utc",
        "git_sha",
        "git_ref",
        "run_kind",
        "board_id",
        "env",
        "lane",
        "suite_or_profile",
        "stress_class",
        "result",
        "metrics_file",
        "scoring_file",
    ]
    missing = [key for key in required if key not in payload]
    if missing:
        raise RuntimeError(f"Manifest missing required fields: {', '.join(missing)}")
    return payload


def load_metrics(manifest_path: Path, manifest: dict[str, Any]) -> list[dict[str, Any]]:
    metrics_path = _resolve_manifest_ref(manifest_path, str(manifest["metrics_file"]))
    records: list[dict[str, Any]] = []
    with metrics_path.open("r", encoding="utf-8") as handle:
        for lineno, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"Invalid metrics NDJSON line {lineno} in {metrics_path}: {exc}") from exc
            if not isinstance(record, dict):
                raise RuntimeError(f"Invalid metrics NDJSON line {lineno} in {metrics_path}: expected object")
            for field in [
                "schema_version",
                "run_id",
                "git_sha",
                "run_kind",
                "suite_or_profile",
                "metric",
                "sample",
                "value",
                "unit",
                "tags",
            ]:
                if field not in record:
                    raise RuntimeError(f"Metric line {lineno} in {metrics_path} missing '{field}'")
            if not isinstance(record.get("tags"), dict):
                raise RuntimeError(f"Metric line {lineno} in {metrics_path} has non-object tags")
            value = record.get("value")
            if isinstance(value, bool):
                value = int(value)
            if not isinstance(value, (int, float)):
                raise RuntimeError(f"Metric line {lineno} in {metrics_path} has non-numeric value")
            record["value"] = float(value)
            records.append(record)
    return records


def _selector_value(selector: dict[str, Any], key: str) -> Any:
    return selector.get(key)


def _selector_matches(policy: MetricPolicy, record: dict[str, Any], manifest: dict[str, Any]) -> bool:
    if policy.run_kind != record.get("run_kind"):
        return False
    if policy.metric != record.get("metric"):
        return False
    selector_suite = _selector_value(policy.selector, "suite_or_profile")
    if selector_suite is not None and selector_suite != record.get("suite_or_profile"):
        return False
    selector_stress = _selector_value(policy.selector, "stress_class")
    if selector_stress is not None and selector_stress != manifest.get("stress_class"):
        return False
    selector_lane = _selector_value(policy.selector, "lane")
    if selector_lane is not None and selector_lane != manifest.get("lane"):
        return False
    return True


def _policy_applies_to_track(policy: MetricPolicy, manifest: dict[str, Any], executed_tracks: set[str]) -> bool:
    if not executed_tracks:
        return False
    if policy.run_kind != manifest.get("run_kind"):
        return False
    selector_stress = _selector_value(policy.selector, "stress_class")
    if selector_stress is not None and selector_stress != manifest.get("stress_class"):
        return False
    selector_lane = _selector_value(policy.selector, "lane")
    if selector_lane is not None and selector_lane != manifest.get("lane"):
        return False
    selector_suite = _selector_value(policy.selector, "suite_or_profile")
    if selector_suite is None:
        return True
    return selector_suite in executed_tracks


def _percentile(values: list[float], pct: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return ordered[lo]
    frac = rank - lo
    return ordered[lo] + (ordered[hi] - ordered[lo]) * frac


def aggregate_metric(policy: MetricPolicy, record_group: list[dict[str, Any]]) -> MetricAggregate:
    values = [float(item["value"]) for item in record_group]
    if not values:
        raise RuntimeError("Cannot aggregate empty metric group")
    if policy.aggregation == "last":
        value = values[-1]
    elif policy.aggregation == "min":
        value = min(values)
    elif policy.aggregation == "max":
        value = max(values)
    elif policy.aggregation == "delta":
        value = values[-1] - values[0] if len(values) > 1 else values[0]
    elif policy.aggregation == "p95":
        p95 = _percentile(values, 95.0)
        if p95 is None:
            raise RuntimeError(f"Cannot compute p95 for metric {policy.metric}")
        value = p95
    else:
        raise RuntimeError(f"Unsupported aggregation '{policy.aggregation}' for metric {policy.metric}")

    first = record_group[0]
    return MetricAggregate(
        metric=policy.metric,
        run_kind=policy.run_kind,
        suite_or_profile=str(first["suite_or_profile"]),
        value=value,
        unit=policy.unit,
        sample_count=len(values),
    )


def _format_num(value: Optional[float]) -> str:
    if value is None:
        return "n/a"
    if abs(value - round(value)) < 1e-9:
        return str(int(round(value)))
    return f"{value:.3f}"


def _pct_delta(current: float, baseline: Optional[float]) -> Optional[float]:
    if baseline is None:
        return None
    if abs(baseline) < 1e-9:
        if abs(current) < 1e-9:
            return 0.0
        return None
    return ((current - baseline) / abs(baseline)) * 100.0


def _absolute_state(policy: MetricPolicy, value: float) -> tuple[str, Optional[str]]:
    if policy.absolute_min is None and policy.absolute_max is None:
        return "n/a", None
    if policy.absolute_min is not None and value < policy.absolute_min:
        return "fail", f"value {_format_num(value)} below min {_format_num(policy.absolute_min)}"
    if policy.absolute_max is not None and value > policy.absolute_max:
        return "fail", f"value {_format_num(value)} above max {_format_num(policy.absolute_max)}"
    return "pass", None


def _regression_state(policy: MetricPolicy, current: float, baseline: Optional[float]) -> tuple[str, str, Optional[str]]:
    if baseline is None:
        return "n/a", "no_baseline", None
    if policy.direction in {"target", "range"}:
        return "n/a", "changed" if abs(current - baseline) > 1e-9 else "unchanged", None
    if policy.regress_abs is None and policy.regress_pct is None:
        if abs(current - baseline) <= 1e-9:
            return "pass", "unchanged", None
        if policy.direction == "lower_better":
            return "pass", "improved" if current < baseline else "changed", None
        return "pass", "improved" if current > baseline else "changed", None

    threshold = 0.0
    if policy.regress_abs is not None:
        threshold = max(threshold, policy.regress_abs)
    if policy.regress_pct is not None:
        threshold = max(threshold, abs(baseline) * policy.regress_pct)

    if policy.direction == "lower_better":
        if current > baseline + threshold:
            return "fail", "regressed", f"value {_format_num(current)} regressed above baseline {_format_num(baseline)} by more than {_format_num(threshold)}"
        if current < baseline - threshold:
            return "pass", "improved", None
        return "pass", "unchanged", None

    if policy.direction == "higher_better":
        if current < baseline - threshold:
            return "fail", "regressed", f"value {_format_num(current)} regressed below baseline {_format_num(baseline)} by more than {_format_num(threshold)}"
        if current > baseline + threshold:
            return "pass", "improved", None
        return "pass", "unchanged", None

    raise RuntimeError(f"Unsupported direction '{policy.direction}' for metric {policy.metric}")


def _track_key(manifest: dict[str, Any]) -> tuple[str, str, str, str]:
    return (
        str(manifest.get("run_kind", "")),
        str(manifest.get("board_id", "")),
        str(manifest.get("env", "")),
        str(manifest.get("stress_class", "")),
    )


def _build_metric_map(
    policies: list[MetricPolicy],
    records: list[dict[str, Any]],
    manifest: dict[str, Any],
) -> tuple[dict[tuple[str, str], MetricAggregate], dict[tuple[str, str], MetricPolicy]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {}
    policy_for_key: dict[tuple[str, str], MetricPolicy] = {}

    for record in records:
        matches = [policy for policy in policies if _selector_matches(policy, record, manifest)]
        if not matches:
            raise RuntimeError(
                "Emitted metric not present in catalog: "
                f"{record.get('run_kind')}/{record.get('suite_or_profile')}/{record.get('metric')}"
            )
        if len(matches) > 1:
            raise RuntimeError(
                "Ambiguous catalog match for emitted metric: "
                f"{record.get('run_kind')}/{record.get('suite_or_profile')}/{record.get('metric')}"
            )
        policy = matches[0]
        key = (str(record["suite_or_profile"]), policy.metric)
        grouped.setdefault(key, []).append(record)
        policy_for_key[key] = policy

    aggregates = {
        key: aggregate_metric(policy_for_key[key], values)
        for key, values in grouped.items()
    }
    return aggregates, policy_for_key


def score_run(
    manifest_path: Path,
    catalog_path: Path,
    baseline_manifest_path: Optional[Path] = None,
) -> dict[str, Any]:
    manifest = load_manifest(manifest_path)
    policies = load_catalog(catalog_path)
    records = load_metrics(manifest_path, manifest)

    current_map, policy_map = _build_metric_map(policies, records, manifest)
    executed_tracks = set(str(item) for item in manifest.get("tracks") or [])
    if not executed_tracks:
        executed_tracks = {str(aggregate.suite_or_profile) for aggregate in current_map.values()}

    baseline_manifest: Optional[dict[str, Any]] = None
    baseline_map: dict[tuple[str, str], MetricAggregate] = {}
    comparison_kind = "no_baseline"
    if baseline_manifest_path is not None and baseline_manifest_path.exists():
        candidate = load_manifest(baseline_manifest_path)
        if _track_key(candidate) == _track_key(manifest):
            baseline_manifest = candidate
            baseline_records = load_metrics(baseline_manifest_path, candidate)
            baseline_map, _ = _build_metric_map(policies, baseline_records, candidate)
            comparison_kind = "run_variance" if candidate.get("git_sha") == manifest.get("git_sha") else "commit_regression"

    metric_results: list[dict[str, Any]] = []
    hard_failures = 0
    advisory_failures = 0
    info_regressions = 0
    missing_required = 0
    missing_optional = 0

    for key in sorted(current_map):
        current = current_map[key]
        policy = policy_map[key]
        baseline = baseline_map.get(key)
        absolute_state, absolute_message = _absolute_state(policy, current.value)
        regression_state, classification, regression_message = _regression_state(
            policy,
            current.value,
            baseline.value if baseline else None,
        )
        score_status = "pass"
        messages = [msg for msg in [absolute_message, regression_message] if msg]

        if absolute_state == "fail":
            if policy.score_level == "hard":
                score_status = "fail"
                hard_failures += 1
            elif policy.score_level == "advisory":
                score_status = "warn"
                advisory_failures += 1
            else:
                score_status = "info"
                info_regressions += 1
        elif regression_state == "fail":
            if policy.score_level == "hard":
                score_status = "fail"
                hard_failures += 1
            elif policy.score_level == "advisory":
                score_status = "warn"
                advisory_failures += 1
            else:
                score_status = "info"
                info_regressions += 1
        elif classification == "regressed" and policy.score_level == "info":
            score_status = "info"
            info_regressions += 1

        metric_results.append(
            {
                "metric": current.metric,
                "run_kind": current.run_kind,
                "suite_or_profile": current.suite_or_profile,
                "unit": current.unit,
                "score_level": policy.score_level,
                "required": policy.required,
                "current_value": current.value,
                "baseline_value": baseline.value if baseline else None,
                "delta_abs": None if baseline is None else current.value - baseline.value,
                "delta_pct": _pct_delta(current.value, baseline.value if baseline else None),
                "sample_count": current.sample_count,
                "classification": classification,
                "absolute_state": absolute_state,
                "regression_state": regression_state,
                "score_status": score_status,
                "messages": messages,
            }
        )

    for policy in policies:
        if not _policy_applies_to_track(policy, manifest, executed_tracks):
            continue
        key = (_selector_value(policy.selector, "suite_or_profile") or manifest["suite_or_profile"], policy.metric)
        if key in current_map:
            continue
        if policy.required:
            score_status = "fail"
        elif policy.score_level == "info":
            score_status = "info"
        else:
            score_status = "warn"
        message = f"metric missing from run output for applicable track ({key[0]})"
        if policy.required:
            hard_failures += 1
            missing_required += 1
        else:
            missing_optional += 1
            if policy.score_level == "info":
                info_regressions += 1
            else:
                advisory_failures += 1
        metric_results.append(
            {
                "metric": policy.metric,
                "run_kind": policy.run_kind,
                "suite_or_profile": key[0],
                "unit": policy.unit,
                "score_level": policy.score_level,
                "required": policy.required,
                "current_value": None,
                "baseline_value": None,
                "delta_abs": None,
                "delta_pct": None,
                "sample_count": 0,
                "classification": "missing",
                "absolute_state": "missing",
                "regression_state": "missing",
                "score_status": score_status,
                "messages": [message],
            }
        )

    base_result = str(manifest.get("base_result", manifest.get("result", "PASS")))
    final_result = "PASS"
    if base_result == "FAIL" or hard_failures > 0:
        final_result = "FAIL"
    elif base_result == "INCONCLUSIVE":
        final_result = "INCONCLUSIVE"
    elif advisory_failures > 0 or base_result == "PASS_WITH_WARNINGS":
        final_result = "PASS_WITH_WARNINGS"
    elif comparison_kind == "no_baseline":
        final_result = "NO_BASELINE"

    return {
        "schema_version": 1,
        "manifest": {
            "path": str(manifest_path),
            "run_id": manifest["run_id"],
            "git_sha": manifest["git_sha"],
            "git_ref": manifest["git_ref"],
            "run_kind": manifest["run_kind"],
            "board_id": manifest["board_id"],
            "env": manifest["env"],
            "lane": manifest["lane"],
            "suite_or_profile": manifest["suite_or_profile"],
            "stress_class": manifest["stress_class"],
            "base_result": base_result,
        },
        "baseline_manifest": None if baseline_manifest is None else {
            "path": str(baseline_manifest_path),
            "run_id": baseline_manifest["run_id"],
            "git_sha": baseline_manifest["git_sha"],
            "git_ref": baseline_manifest["git_ref"],
        },
        "comparison_kind": comparison_kind,
        "result": final_result,
        "summary": {
            "metrics_scored": len(metric_results),
            "hard_failures": hard_failures,
            "advisory_failures": advisory_failures,
            "info_regressions": info_regressions,
            "missing_required": missing_required,
            "missing_optional": missing_optional,
        },
        "metrics": sorted(
            metric_results,
            key=lambda item: (
                0 if item["score_status"] == "fail" else 1 if item["score_status"] == "warn" else 2,
                str(item["suite_or_profile"]),
                str(item["metric"]),
            ),
        ),
    }


def render_human(result: dict[str, Any]) -> str:
    manifest = result["manifest"]
    baseline = result.get("baseline_manifest")
    lines = [
        "# Hardware Run Score",
        "",
        f"- Result: **{result['result']}**",
        f"- Base result: `{manifest['base_result']}`",
        f"- Run kind: `{manifest['run_kind']}`",
        f"- Lane: `{manifest['lane']}`",
        f"- Track: `{manifest['suite_or_profile']}`",
        f"- Stress class: `{manifest['stress_class']}`",
        f"- Board: `{manifest['board_id']}`",
        f"- Git: `{manifest['git_sha']}` ({manifest['git_ref']})",
        f"- Comparison: `{result['comparison_kind']}`",
    ]
    if baseline:
        lines.append(f"- Baseline git: `{baseline['git_sha']}` ({baseline['git_ref']})")

    summary = result["summary"]
    lines.extend(
        [
            "",
            "## Summary",
            "",
            f"- Metrics scored: {summary['metrics_scored']}",
            f"- Hard failures: {summary['hard_failures']}",
            f"- Advisory failures: {summary['advisory_failures']}",
            f"- Info regressions: {summary['info_regressions']}",
            f"- Missing required: {summary['missing_required']}",
            f"- Missing optional: {summary['missing_optional']}",
            "",
            "## Metrics",
            "",
            "| Status | Track | Metric | Current | Baseline | Delta | Delta % | Classification |",
            "|--------|-------|--------|--------:|---------:|------:|--------:|----------------|",
        ]
    )
    noteworthy: list[str] = []
    for metric in result["metrics"]:
        delta_pct = _format_num(metric["delta_pct"])
        if delta_pct != "n/a":
            delta_pct = f"{delta_pct}%"
        lines.append(
            "| "
            f"{metric['score_status'].upper()} | "
            f"`{metric['suite_or_profile']}` | "
            f"`{metric['metric']}` | "
            f"{_format_num(metric['current_value'])} | "
            f"{_format_num(metric['baseline_value'])} | "
            f"{_format_num(metric['delta_abs'])} | "
            f"{delta_pct} | "
            f"{metric['classification']} |"
        )
        for message in metric["messages"]:
            noteworthy.append(f"- `{metric['suite_or_profile']}` / `{metric['metric']}`: {message}")
    if noteworthy:
        lines.extend(["", "## Findings", ""])
        lines.extend(noteworthy)
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    manifest_path = Path(args.manifest).resolve()
    catalog_path = Path(args.catalog).resolve()
    baseline_path = Path(args.compare_to).resolve() if args.compare_to else None

    try:
        result = score_run(manifest_path, catalog_path, baseline_path)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(render_human(result), end="")

    final_result = str(result["result"])
    if final_result == "FAIL":
        return 2
    if final_result == "PASS_WITH_WARNINGS":
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
