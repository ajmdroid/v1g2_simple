#!/usr/bin/env python3
"""Validate docs/PERF_SLOS.md tables against tools/perf_slo_thresholds.json."""

from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC_PATH = ROOT / "docs" / "PERF_SLOS.md"
SLO_PATH = ROOT / "tools" / "perf_slo_thresholds.json"

HARD_HEADING = "## Hard SLOs (Must Pass)"
ADVISORY_HEADING = "## Advisory SLOs (Track/Trend)"
HARD_HEADER_PREFIX = "| Metric | Rule | `drive_wifi_off` | `drive_wifi_ap` |"
ADVISORY_HEADER_PREFIX = "| Metric | Rule | Limit |"


def read_lines(path: Path) -> list[str]:
    if not path.exists():
        raise FileNotFoundError(f"File not found: {path}")
    return path.read_text(encoding="utf-8").splitlines()


def heading_index(lines: list[str], heading: str) -> int:
    for idx, line in enumerate(lines):
        if line.strip() == heading:
            return idx
    raise RuntimeError(f"Heading not found in {DOC_PATH}: {heading}")


def find_table_header(lines: list[str], start: int, header_prefix: str) -> int:
    for idx in range(start, len(lines)):
        if lines[idx].strip().startswith(header_prefix):
            return idx
    raise RuntimeError(f"Table header not found: {header_prefix}")


def parse_table_row(line: str) -> list[str]:
    parts = [part.strip() for part in line.strip().strip("|").split("|")]
    return parts


def normalize_rule(rule: str) -> str:
    cleaned = " ".join(rule.strip().split())
    cleaned = re.sub(r"\s*/\s*", "/", cleaned)
    return cleaned


def parse_limit(value: str) -> float:
    raw = value.strip().strip("`").replace(",", "")
    if not raw:
        raise ValueError("empty limit value")
    return float(raw)


def format_limit(value: float) -> str:
    if math.isclose(value, round(value), rel_tol=0.0, abs_tol=1e-9):
        return str(int(round(value)))
    return f"{value:.6g}"


def parse_hard_doc_table(lines: list[str]) -> dict[str, tuple[str, float, float]]:
    out: dict[str, tuple[str, float, float]] = {}
    start = heading_index(lines, HARD_HEADING)
    table_header_idx = find_table_header(lines, start, HARD_HEADER_PREFIX)

    for line in lines[table_header_idx + 2 :]:
        stripped = line.strip()
        if not stripped.startswith("|"):
            break
        cols = parse_table_row(stripped)
        if len(cols) < 4:
            continue
        metric = cols[0].strip("`")
        rule = normalize_rule(cols[1])
        off = parse_limit(cols[2])
        ap = parse_limit(cols[3])
        out[metric] = (rule, off, ap)
    return out


def parse_advisory_doc_table(lines: list[str]) -> dict[str, tuple[str, float]]:
    out: dict[str, tuple[str, float]] = {}
    start = heading_index(lines, ADVISORY_HEADING)
    table_header_idx = find_table_header(lines, start, ADVISORY_HEADER_PREFIX)

    for line in lines[table_header_idx + 2 :]:
        stripped = line.strip()
        if not stripped.startswith("|"):
            break
        cols = parse_table_row(stripped)
        if len(cols) < 3:
            continue
        metric = cols[0].strip("`")
        rule = normalize_rule(cols[1])
        limit = parse_limit(cols[2])
        out[metric] = (rule, limit)
    return out


def build_expected_hard(payload: dict) -> dict[str, tuple[str, float, float]]:
    profiles = payload.get("profiles", [])
    if "drive_wifi_off" not in profiles or "drive_wifi_ap" not in profiles:
        raise RuntimeError("Expected profiles drive_wifi_off and drive_wifi_ap in perf_slo_thresholds.json")

    out: dict[str, tuple[str, float, float]] = {}
    for row in payload.get("hard_common", []):
        metric = str(row["metric"])
        rule = normalize_rule(f"{row['source']} {row['op']}")
        limit = float(row["limit"])
        out[metric] = (rule, limit, limit)

    by_profile: dict[str, dict[str, tuple[str, str, float]]] = {}
    for profile in ("drive_wifi_off", "drive_wifi_ap"):
        entries = payload.get("hard_profile", {}).get(profile, [])
        for row in entries:
            metric = str(row["metric"])
            by_profile.setdefault(metric, {})
            by_profile[metric][profile] = (
                str(row["source"]),
                str(row["op"]),
                float(row["limit"]),
            )

    for metric, profile_rows in by_profile.items():
        if "drive_wifi_off" not in profile_rows or "drive_wifi_ap" not in profile_rows:
            raise RuntimeError(f"hard_profile metric '{metric}' must exist for both profiles")
        off_source, off_op, off_limit = profile_rows["drive_wifi_off"]
        ap_source, ap_op, ap_limit = profile_rows["drive_wifi_ap"]
        if off_source == ap_source and off_op == ap_op:
            rule = f"{off_source} {off_op}"
        elif off_source == ap_source:
            rule = f"{off_source} {off_op} / {ap_op}"
        else:
            rule = f"{off_source} {off_op} / {ap_source} {ap_op}"
        out[metric] = (normalize_rule(rule), off_limit, ap_limit)

    return out


def build_expected_advisory(payload: dict) -> dict[str, tuple[str, float]]:
    out: dict[str, tuple[str, float]] = {}
    for row in payload.get("advisory", []):
        metric = str(row["metric"])
        rule = normalize_rule(f"{row['source']} {row['op']}")
        limit = float(row["limit"])
        out[metric] = (rule, limit)
    return out


def compare_hard(
    expected: dict[str, tuple[str, float, float]],
    actual: dict[str, tuple[str, float, float]],
) -> list[str]:
    errors: list[str] = []
    expected_metrics = set(expected)
    actual_metrics = set(actual)

    missing = sorted(expected_metrics - actual_metrics)
    extra = sorted(actual_metrics - expected_metrics)
    if missing:
        errors.append("Hard table missing metrics: " + ", ".join(missing))
    if extra:
        errors.append("Hard table has extra metrics: " + ", ".join(extra))

    for metric in sorted(expected_metrics & actual_metrics):
        expected_rule, expected_off, expected_ap = expected[metric]
        actual_rule, actual_off, actual_ap = actual[metric]
        if normalize_rule(actual_rule) != normalize_rule(expected_rule):
            errors.append(
                f"Hard rule mismatch for {metric}: doc='{actual_rule}' expected='{expected_rule}'"
            )
        if not math.isclose(actual_off, expected_off, rel_tol=0.0, abs_tol=1e-9):
            errors.append(
                f"Hard drive_wifi_off limit mismatch for {metric}: doc={format_limit(actual_off)} expected={format_limit(expected_off)}"
            )
        if not math.isclose(actual_ap, expected_ap, rel_tol=0.0, abs_tol=1e-9):
            errors.append(
                f"Hard drive_wifi_ap limit mismatch for {metric}: doc={format_limit(actual_ap)} expected={format_limit(expected_ap)}"
            )
    return errors


def compare_advisory(
    expected: dict[str, tuple[str, float]],
    actual: dict[str, tuple[str, float]],
) -> list[str]:
    errors: list[str] = []
    expected_metrics = set(expected)
    actual_metrics = set(actual)

    missing = sorted(expected_metrics - actual_metrics)
    extra = sorted(actual_metrics - expected_metrics)
    if missing:
        errors.append("Advisory table missing metrics: " + ", ".join(missing))
    if extra:
        errors.append("Advisory table has extra metrics: " + ", ".join(extra))

    for metric in sorted(expected_metrics & actual_metrics):
        expected_rule, expected_limit = expected[metric]
        actual_rule, actual_limit = actual[metric]
        if normalize_rule(actual_rule) != normalize_rule(expected_rule):
            errors.append(
                f"Advisory rule mismatch for {metric}: doc='{actual_rule}' expected='{expected_rule}'"
            )
        if not math.isclose(actual_limit, expected_limit, rel_tol=0.0, abs_tol=1e-9):
            errors.append(
                f"Advisory limit mismatch for {metric}: doc={format_limit(actual_limit)} expected={format_limit(expected_limit)}"
            )
    return errors


def main() -> int:
    try:
        payload = json.loads(SLO_PATH.read_text(encoding="utf-8"))
        doc_lines = read_lines(DOC_PATH)
        expected_hard = build_expected_hard(payload)
        expected_advisory = build_expected_advisory(payload)
        actual_hard = parse_hard_doc_table(doc_lines)
        actual_advisory = parse_advisory_doc_table(doc_lines)
    except Exception as exc:
        print(f"[contract] perf-slo: {exc}")
        return 1

    errors = compare_hard(expected_hard, actual_hard)
    errors.extend(compare_advisory(expected_advisory, actual_advisory))
    if errors:
        print("[contract] perf-slo threshold/doc mismatch:")
        for row in errors:
            print(f"  - {row}")
        return 1

    print(
        "[contract] perf-slo thresholds match docs "
        f"({len(actual_hard)} hard metrics, {len(actual_advisory)} advisory metrics)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
