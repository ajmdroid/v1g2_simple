#!/usr/bin/env python3
"""Check WiFi API route/policy contracts for extracted ApiService endpoints.

This script enforces three invariants in src/wifi_manager.cpp:
1) Route contract for extracted API modules stays stable (method + path).
2) Route-lambda policy contract for ApiService endpoints stays stable
   (rate-limit, UI activity mark, OBD enabled gate, delegate calls).
3) Legacy /api/lockout/* compatibility routes preserve deprecation headers
   that point callers to /api/lockouts/*.

Use --update to rewrite expected contract snapshots from current source.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple

ROOT = Path(__file__).resolve().parents[1]
SRC_FILE = ROOT / "src" / "wifi_manager.cpp"
ROUTE_CONTRACT_FILE = ROOT / "test" / "contracts" / "wifi_route_contract.txt"
POLICY_CONTRACT_FILE = ROOT / "test" / "contracts" / "wifi_handler_policy_contract.txt"
LEGACY_LOCKOUT_CONTRACT_FILE = (
    ROOT / "test" / "contracts" / "wifi_legacy_lockout_contract.txt"
)

ROUTE_PREFIXES = (
    "/api/settings/backup",
    "/api/settings/restore",
    "/api/debug/",
    "/api/obd/",
    "/api/gps/",
    "/api/cameras/",
    "/api/lockouts/",
    "/api/lockout/",
)

ROUTE_SIGNATURE_RE = re.compile(r'server\.on\("([^"]+)",\s*(HTTP_[A-Z]+),')
ROUTE_LAMBDA_START_RE = re.compile(
    r'server\.on\("([^"]+)",\s*(HTTP_[A-Z]+),\s*\[this\]\(\)\s*\{'
)
DELEGATE_RE = re.compile(r"([A-Za-z]+ApiService::[A-Za-z0-9_]+)\s*\(")
DEPRECATED_HEADER_RE = re.compile(
    r'server\.sendHeader\(\s*"X-API-Deprecated"\s*,\s*"Use\s+([^"]+)"\s*\)'
)


@dataclass(frozen=True)
class RoutePolicy:
    route: str
    rate_limit: int
    ui_activity: int
    obd_enabled_gate: int
    delegates: Tuple[str, ...]

    def to_line(self) -> str:
        delegate_blob = ",".join(self.delegates)
        return (
            f"route={self.route} "
            f"rate_limit={self.rate_limit} "
            f"ui_activity={self.ui_activity} "
            f"obd_enabled_gate={self.obd_enabled_gate} "
            f"delegates={delegate_blob}"
        )


def read_source() -> str:
    if not SRC_FILE.exists():
        raise FileNotFoundError(f"Source file not found: {SRC_FILE}")
    return SRC_FILE.read_text(encoding="utf-8")


def extract_routes(source: str) -> List[str]:
    rows: List[str] = []
    for path, method in ROUTE_SIGNATURE_RE.findall(source):
        if path.startswith(ROUTE_PREFIXES):
            rows.append(f"{method} {path}")
    # Keep deterministic ordering independent of registration line moves.
    return sorted(set(rows))


def find_matching_brace(source: str, open_brace_index: int) -> int:
    depth = 0
    for idx in range(open_brace_index, len(source)):
        ch = source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    raise ValueError("Unbalanced braces while parsing route lambda body")


def extract_route_lambda_bodies(source: str) -> Dict[str, str]:
    routes: Dict[str, str] = {}
    for match in ROUTE_LAMBDA_START_RE.finditer(source):
        path = match.group(1)
        method = match.group(2)
        if not path.startswith(ROUTE_PREFIXES):
            continue

        route = f"{method} {path}"
        open_idx = match.end() - 1
        close_idx = find_matching_brace(source, open_idx)
        routes[route] = source[open_idx + 1 : close_idx]
    return routes


def extract_policy_contract(source: str) -> List[RoutePolicy]:
    routes = extract_route_lambda_bodies(source)
    out: List[RoutePolicy] = []

    for route, body in routes.items():
        delegates = tuple(sorted(set(DELEGATE_RE.findall(body))))
        if not delegates:
            continue

        has_rate = int("checkRateLimit(" in body)
        has_ui = int("markUiActivity(" in body)
        has_obd_gate = int(
            "settingsManager.get().obdEnabled" in body and "server.send(409" in body
        )

        out.append(
            RoutePolicy(
                route=route,
                rate_limit=has_rate,
                ui_activity=has_ui,
                obd_enabled_gate=has_obd_gate,
                delegates=delegates,
            )
        )

    out.sort(key=lambda p: p.route)
    return out


def extract_legacy_lockout_contract(source: str) -> List[str]:
    routes = extract_route_lambda_bodies(source)
    out: List[str] = []

    for route, body in routes.items():
        _method, path = route.split(" ", 1)
        if not path.startswith("/api/lockout/"):
            continue

        header = DEPRECATED_HEADER_RE.search(body)
        has_header = int(header is not None)
        target = header.group(1) if header else ""
        out.append(
            f"route={route} "
            f"deprecated_header={has_header} "
            f"deprecated_target={target}"
        )

    return sorted(out)


def read_expected_lines(path: Path) -> List[str]:
    if not path.exists():
        return []
    lines: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        lines.append(line)
    return lines


def write_lines(path: Path, header: str, lines: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = [header, ""]
    payload.extend(lines)
    payload.append("")
    path.write_text("\n".join(payload), encoding="utf-8")


def print_diff(expected: List[str], actual: List[str], label: str) -> None:
    expected_set = set(expected)
    actual_set = set(actual)

    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)

    print(f"[contract] {label} mismatch")
    if missing:
        print("  missing:")
        for row in missing:
            print(f"    - {row}")
    if extra:
        print("  extra:")
        for row in extra:
            print(f"    + {row}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite expected contract files from current source",
    )
    args = parser.parse_args()

    source = read_source()

    routes = extract_routes(source)
    policies = extract_policy_contract(source)
    policy_lines = [p.to_line() for p in policies]
    legacy_lockout_lines = extract_legacy_lockout_contract(source)

    if args.update:
        write_lines(
            ROUTE_CONTRACT_FILE,
            "# WiFi API route contract (extracted ApiService endpoints)",
            routes,
        )
        write_lines(
            POLICY_CONTRACT_FILE,
            "# WiFi API route policy contract (ApiService endpoint lambdas)",
            policy_lines,
        )
        write_lines(
            LEGACY_LOCKOUT_CONTRACT_FILE,
            "# WiFi API legacy lockout compatibility contract",
            legacy_lockout_lines,
        )
        print(f"Updated {ROUTE_CONTRACT_FILE}")
        print(f"Updated {POLICY_CONTRACT_FILE}")
        print(f"Updated {LEGACY_LOCKOUT_CONTRACT_FILE}")
        return 0

    expected_routes = read_expected_lines(ROUTE_CONTRACT_FILE)
    expected_policy = read_expected_lines(POLICY_CONTRACT_FILE)
    expected_legacy_lockout = read_expected_lines(LEGACY_LOCKOUT_CONTRACT_FILE)

    ok = True

    if expected_routes != routes:
        print_diff(expected_routes, routes, "route")
        ok = False
    if expected_policy != policy_lines:
        print_diff(expected_policy, policy_lines, "policy")
        ok = False
    if expected_legacy_lockout != legacy_lockout_lines:
        print_diff(expected_legacy_lockout, legacy_lockout_lines, "legacy-lockout")
        ok = False

    if not ok:
        print("\nRun with --update only when intentionally changing contract.")
        return 1

    print("[contract] route, policy, and legacy lockout contracts match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
