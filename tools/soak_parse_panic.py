#!/usr/bin/env python3
"""Parse soak panic JSONL into key=value summary fields."""

import json
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: soak_parse_panic.py <panic_jsonl>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    samples = 0
    ok_samples = 0
    was_crash_true = 0
    has_panic_file_true = 0
    last_reset_reason = ""

    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                samples += 1
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if not rec.get("ok"):
                    continue
                data = rec.get("data")
                if not isinstance(data, dict):
                    continue
                ok_samples += 1
                if data.get("wasCrash") is True:
                    was_crash_true += 1
                if data.get("hasPanicFile") is True:
                    has_panic_file_true += 1
                lr = data.get("lastResetReason")
                if isinstance(lr, str) and lr:
                    last_reset_reason = lr
    except FileNotFoundError:
        pass

    print(f"samples={samples}")
    print(f"ok_samples={ok_samples}")
    print(f"was_crash_true={was_crash_true}")
    print(f"has_panic_file_true={has_panic_file_true}")
    print(f"last_reset_reason={last_reset_reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
