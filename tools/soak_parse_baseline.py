#!/usr/bin/env python3
"""Derive soak gates from a baseline perf CSV session."""

import csv
import math
import sys


def iv(row: dict[str, str], key: str) -> int:
    try:
        return int(row.get(key, "0") or "0")
    except Exception:
        return 0


def main() -> int:
    if len(sys.argv) != 6:
        print(
            "usage: soak_parse_baseline.py <perf_csv> <session_mode> <target_duration_s> <latency_factor> <throughput_factor>",
            file=sys.stderr,
        )
        return 2

    path = sys.argv[1]
    session_mode = sys.argv[2]
    try:
        target_duration_s = int(sys.argv[3])
        latency_factor = float(sys.argv[4])
        throughput_factor = float(sys.argv[5])
    except ValueError as exc:
        raise SystemExit(f"invalid numeric baseline argument: {exc}")

    session_rows: dict[int, list[dict[str, str]]] = {}
    session_index = 0
    header: list[str] | None = None

    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        reader = csv.reader(fh)
        for row in reader:
            if not row:
                continue
            first = row[0].strip()
            if first.startswith("#session_start"):
                session_index += 1
                continue
            if first == "millis":
                header = row
                continue
            if header is None:
                continue
            if len(row) < len(header):
                row = row + [""] * (len(header) - len(row))
            if session_index == 0:
                session_index = 1
            record = dict(zip(header, row))
            session_rows.setdefault(session_index, []).append(record)

    if not session_rows:
        raise SystemExit("no session rows parsed from baseline perf CSV")

    sessions = sorted(session_rows.items(), key=lambda item: item[0])
    connected = [(idx, rows) for idx, rows in sessions if rows and iv(rows[-1], "rx") > 0]

    selected = None
    if session_mode == "last-connected":
        selected = connected[-1] if connected else sessions[-1]
    elif session_mode == "last":
        selected = sessions[-1]
    elif session_mode == "longest-connected":
        source = connected if connected else sessions
        selected = max(source, key=lambda item: max(0, iv(item[1][-1], "millis") - iv(item[1][0], "millis")))
    elif session_mode == "longest":
        selected = max(sessions, key=lambda item: max(0, iv(item[1][-1], "millis") - iv(item[1][0], "millis")))
    elif session_mode.isdigit():
        wanted = int(session_mode)
        selected = next((item for item in sessions if item[0] == wanted), None)
        if selected is None:
            raise SystemExit(f"session index {wanted} not found")
    else:
        raise SystemExit(
            f"unsupported --baseline-perf-session '{session_mode}' (use last-connected, last, longest-connected, longest, or index)"
        )

    sel_index, rows = selected
    if not rows:
        raise SystemExit("selected baseline session has no rows")

    duration_ms = max(1, iv(rows[-1], "millis") - iv(rows[0], "millis"))
    duration_s = duration_ms / 1000.0
    if duration_s <= 0:
        duration_s = 0.001

    rx_delta = max(0, iv(rows[-1], "rx") - iv(rows[0], "rx"))
    parse_delta = max(0, iv(rows[-1], "parseOK") - iv(rows[0], "parseOK"))

    rx_rate = rx_delta / duration_s
    parse_rate = parse_delta / duration_s

    derived_min_rx = max(1, int(math.floor(rx_rate * target_duration_s * throughput_factor)))
    derived_min_parse = max(1, int(math.floor(parse_rate * target_duration_s * throughput_factor)))

    peak_loop = max(iv(r, "loopMax_us") for r in rows)
    peak_flush = max(iv(r, "flushMax_us") for r in rows)
    peak_wifi = max(iv(r, "wifiMax_us") for r in rows)
    peak_ble_drain = max(iv(r, "bleDrainMax_us") for r in rows)

    def derive_peak_limit(peak_value: int) -> int:
        if peak_value <= 0:
            return 0
        return max(1, int(math.ceil(peak_value * latency_factor)))

    print(f"session_index={sel_index}")
    print(f"rows={len(rows)}")
    print(f"duration_ms={duration_ms}")
    print(f"rx_rate_per_sec={rx_rate:.6f}")
    print(f"parse_rate_per_sec={parse_rate:.6f}")
    print(f"peak_loop_us={peak_loop}")
    print(f"peak_flush_us={peak_flush}")
    print(f"peak_wifi_us={peak_wifi}")
    print(f"peak_ble_drain_us={peak_ble_drain}")
    print(f"derived_min_rx_delta={derived_min_rx}")
    print(f"derived_min_parse_delta={derived_min_parse}")
    print(f"derived_max_loop_us={derive_peak_limit(peak_loop)}")
    print(f"derived_max_flush_us={derive_peak_limit(peak_flush)}")
    print(f"derived_max_wifi_us={derive_peak_limit(peak_wifi)}")
    print(f"derived_max_ble_drain_us={derive_peak_limit(peak_ble_drain)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
