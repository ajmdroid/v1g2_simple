#!/usr/bin/env python3
"""Check CI lane elapsed time against budgets defined in tools/ci_time_budgets.json.

Usage:
    python3 scripts/check_ci_budget.py <lane> <timing-json>

Where <lane> is one of: ci-test, build-yml, nightly, pre-release
and <timing-json> is a path to a JSON file with {"elapsed_seconds": N}.

Exit codes:
    0 = within budget
    1 = over budget
    2 = usage / input error
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUDGETS_PATH = ROOT / "tools" / "ci_time_budgets.json"


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <lane> <timing-json>", file=sys.stderr)
        return 2

    lane = sys.argv[1]
    timing_path = Path(sys.argv[2])

    if not BUDGETS_PATH.exists():
        print(f"Budget file not found: {BUDGETS_PATH}", file=sys.stderr)
        return 2

    with open(BUDGETS_PATH) as f:
        budgets = json.load(f)["budgets"]

    if lane not in budgets:
        print(f"Unknown lane '{lane}'. Valid: {', '.join(budgets)}", file=sys.stderr)
        return 2

    if not timing_path.exists():
        print(f"Timing file not found: {timing_path}", file=sys.stderr)
        return 2

    with open(timing_path) as f:
        timing = json.load(f)

    elapsed = timing.get("elapsed_seconds")
    if elapsed is None:
        print("Timing JSON missing 'elapsed_seconds' key.", file=sys.stderr)
        return 2

    max_seconds = budgets[lane]["max_seconds"]
    if elapsed > max_seconds:
        print(
            f"OVER BUDGET: {lane} took {elapsed}s, budget is {max_seconds}s "
            f"({elapsed - max_seconds}s over)"
        )
        return 1

    print(f"Within budget: {lane} took {elapsed}s / {max_seconds}s limit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
