#!/usr/bin/env python3
"""
V1G2 Simple Stability Scorecard (debug utility)

Evaluates NDJSON debug logs against the Base Stable standard defined in
this file's threshold constants.

This tool is retained for exploratory analysis only. The authoritative perf
gate is `tools/score_perf_csv.py` plus the thresholds in
`tools/perf_slo_thresholds.json`.

Usage:
    python tools/scorecard.py test/fixtures/debug_base_stable.log
    python tools/scorecard.py test/fixtures/debug_base_stable.log --verbose
    python tools/scorecard.py test/fixtures/debug_base_stable.log --json

Exit codes:
    0 = GREEN (all pass)
    1 = YELLOW (warnings)
    2 = RED (failures)
    3 = ERROR (tool error)
"""

import argparse
import json
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Any

# =============================================================================
# Stability Standard Thresholds (maintained in this file)
# =============================================================================

# Steady-state is defined as millis >= this value
STEADY_STATE_MILLIS = 60_000

# SLO thresholds (microseconds unless noted)
SLO_LOOP_MAX_US = 500_000       # 500ms
SLO_BLE_DRAIN_MAX_US = 200_000  # 200ms
SLO_WIFI_MAX_US = 150_000       # 150ms
SLO_FS_MAX_US = 50_000          # 50ms
SLO_SD_MAX_US = 50_000          # 50ms

# Reconnect window: relaxed thresholds during reconnect
RECONNECT_LOOP_MAX_US = 5_000_000  # 5s allowed during reconnect
RECONNECT_WINDOW_MS = 30_000       # 30s after reconnect increments

# Attribution threshold: a metric "explains" loopMax if it's >= 90% of loopMax
ATTRIBUTION_THRESHOLD = 0.90


@dataclass
class PerfRecord:
    """Parsed performance metrics record."""
    millis: int = 0
    rx: int = 0
    q_drop: int = 0
    q_hw: int = 0
    parse_ok: int = 0
    parse_fail: int = 0
    disc: int = 0
    reconn: int = 0
    loop_max_us: int = 0
    wifi_max_us: int = 0
    fs_max_us: int = 0
    sd_max_us: int = 0
    ble_drain_max_us: int = 0
    ble_conn_max_us: int = 0
    ble_disc_max_us: int = 0
    ble_subs_max_us: int = 0
    ble_process_max_us: int = 0
    flush_max_us: int = 0
    display_render_max_us: int = 0
    heap_min: int = 0
    raw: Dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_json(cls, data: Dict[str, Any]) -> "PerfRecord":
        """Parse a perf record from JSON dict."""
        return cls(
            millis=data.get("millis", 0),
            rx=data.get("rx", 0),
            q_drop=data.get("qDrop", 0),
            q_hw=data.get("qHW", 0),
            parse_ok=data.get("parseOK", 0),
            parse_fail=data.get("parseFail", 0),
            disc=data.get("disc", 0),
            reconn=data.get("reconn", 0),
            loop_max_us=data.get("loopMax_us", 0),
            wifi_max_us=data.get("wifiMax_us", 0),
            fs_max_us=data.get("fsMax_us", 0),
            sd_max_us=data.get("sdMax_us", 0),
            ble_drain_max_us=data.get("bleDrainMax_us", 0),
            ble_conn_max_us=data.get("bleConnMax_us", 0),
            ble_disc_max_us=data.get("bleDiscMax_us", 0),
            ble_subs_max_us=data.get("bleSubsMax_us", 0),
            ble_process_max_us=data.get("bleProcessMax_us", 0),
            flush_max_us=data.get("flushMax_us", 0),
            display_render_max_us=data.get("displayRenderMax_us", 0),
            heap_min=data.get("heapMin", 0),
            raw=data,
        )

    def is_steady_state(self) -> bool:
        """Check if this record is in steady-state (past boot)."""
        return self.millis >= STEADY_STATE_MILLIS

    def get_attribution_metrics(self) -> Dict[str, int]:
        """Return metrics that can explain loopMax_us."""
        return {
            "bleConnMax_us": self.ble_conn_max_us,
            "bleDiscMax_us": self.ble_disc_max_us,
            "bleSubsMax_us": self.ble_subs_max_us,
            "bleDrainMax_us": self.ble_drain_max_us,
            "wifiMax_us": self.wifi_max_us,
            "fsMax_us": self.fs_max_us,
            "sdMax_us": self.sd_max_us,
            "flushMax_us": self.flush_max_us,
            "displayRenderMax_us": self.display_render_max_us,
        }

    def find_attribution(self) -> Optional[str]:
        """Find which metric explains loopMax_us, if any."""
        if self.loop_max_us == 0:
            return None
        threshold = self.loop_max_us * ATTRIBUTION_THRESHOLD
        for name, value in self.get_attribution_metrics().items():
            if value >= threshold:
                return name
        return None


@dataclass
class ScorecardResult:
    """Results of scorecard evaluation."""
    total_lines: int = 0
    parse_errors: int = 0
    perf_records: int = 0
    steady_state_records: int = 0
    
    # Invariants
    total_q_drop: int = 0
    total_parse_fail: int = 0
    total_disc: int = 0
    max_reconn: int = 0
    
    # SLO tracking
    max_loop_us: int = 0
    max_ble_drain_us: int = 0
    max_wifi_us: int = 0
    max_fs_us: int = 0
    max_sd_us: int = 0
    max_ble_conn_us: int = 0
    max_ble_disc_us: int = 0
    max_ble_subs_us: int = 0
    
    # Mystery stalls
    mystery_stalls: int = 0
    
    # Worst samples
    worst_samples: List[PerfRecord] = field(default_factory=list)
    
    # Reconnect tracking
    reconnect_detected: bool = False
    reconnect_millis: int = 0
    
    def is_in_reconnect_window(self, millis: int) -> bool:
        """Check if we're within the reconnect tolerance window."""
        if not self.reconnect_detected:
            return False
        return millis < self.reconnect_millis + RECONNECT_WINDOW_MS
    
    def invariants_pass(self) -> bool:
        """Check if all invariants pass."""
        return (
            self.total_q_drop == 0 and
            self.total_parse_fail == 0 and
            self.total_disc == 0 and  # disc should be 0 in steady-state
            self.parse_errors == 0
        )
    
    def slo_violations(self) -> List[str]:
        """Return list of SLO violations."""
        violations = []
        if self.max_loop_us > SLO_LOOP_MAX_US:
            violations.append(f"loopMax_us={self.max_loop_us} > {SLO_LOOP_MAX_US}")
        if self.max_ble_drain_us > SLO_BLE_DRAIN_MAX_US:
            violations.append(f"bleDrainMax_us={self.max_ble_drain_us} > {SLO_BLE_DRAIN_MAX_US}")
        if self.max_wifi_us > SLO_WIFI_MAX_US:
            violations.append(f"wifiMax_us={self.max_wifi_us} > {SLO_WIFI_MAX_US}")
        if self.max_fs_us > SLO_FS_MAX_US:
            violations.append(f"fsMax_us={self.max_fs_us} > {SLO_FS_MAX_US}")
        if self.max_sd_us > SLO_SD_MAX_US:
            violations.append(f"sdMax_us={self.max_sd_us} > {SLO_SD_MAX_US}")
        return violations
    
    def grade(self) -> str:
        """Determine overall grade."""
        if not self.invariants_pass():
            return "RED"
        if self.mystery_stalls > 0:
            return "RED"
        
        violations = self.slo_violations()
        if len(violations) > 2:
            return "RED"
        if len(violations) > 0 or self.reconnect_detected:
            return "YELLOW"
        
        return "GREEN"


def parse_log(filepath: str) -> tuple[ScorecardResult, List[PerfRecord]]:
    """Parse log file and return scorecard result and all perf records."""
    result = ScorecardResult()
    records: List[PerfRecord] = []
    prev_reconn = 0
    
    with open(filepath, "r") as f:
        for line in f:
            result.total_lines += 1
            line = line.strip()
            if not line:
                continue
            
            # Try to parse as JSON
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                # Not JSON - might be a breadcrumb or other log line, skip silently
                continue
            
            # Check if this is a perf record
            if data.get("category") != "perf":
                continue
            
            result.perf_records += 1
            
            try:
                record = PerfRecord.from_json(data)
                records.append(record)
            except Exception:
                result.parse_errors += 1
                continue
            
            # Detect reconnect
            if record.reconn > prev_reconn:
                result.reconnect_detected = True
                result.reconnect_millis = record.millis
            prev_reconn = record.reconn
            
            # Skip non-steady-state records for SLO evaluation
            if not record.is_steady_state():
                continue
            
            result.steady_state_records += 1
            
            # Track invariants (cumulative from perf records)
            result.total_q_drop = max(result.total_q_drop, record.q_drop)
            result.total_parse_fail = max(result.total_parse_fail, record.parse_fail)
            
            # disc in steady-state should be 0 (outside reconnect window)
            if not result.is_in_reconnect_window(record.millis):
                result.total_disc = max(result.total_disc, record.disc)
            
            result.max_reconn = max(result.max_reconn, record.reconn)
            
            # Track SLOs (only outside reconnect window)
            if not result.is_in_reconnect_window(record.millis):
                result.max_loop_us = max(result.max_loop_us, record.loop_max_us)
                result.max_ble_drain_us = max(result.max_ble_drain_us, record.ble_drain_max_us)
                result.max_wifi_us = max(result.max_wifi_us, record.wifi_max_us)
                result.max_fs_us = max(result.max_fs_us, record.fs_max_us)
                result.max_sd_us = max(result.max_sd_us, record.sd_max_us)
            
            # Always track BLE connection metrics
            result.max_ble_conn_us = max(result.max_ble_conn_us, record.ble_conn_max_us)
            result.max_ble_disc_us = max(result.max_ble_disc_us, record.ble_disc_max_us)
            result.max_ble_subs_us = max(result.max_ble_subs_us, record.ble_subs_max_us)
            
            # Check for mystery stalls (loopMax exceeds threshold but no attribution)
            threshold = RECONNECT_LOOP_MAX_US if result.is_in_reconnect_window(record.millis) else SLO_LOOP_MAX_US
            if record.loop_max_us > threshold:
                if record.find_attribution() is None:
                    result.mystery_stalls += 1
    
    # Find worst samples (top 5 by loopMax_us in steady-state)
    steady_records = [r for r in records if r.is_steady_state()]
    steady_records.sort(key=lambda r: r.loop_max_us, reverse=True)
    result.worst_samples = steady_records[:5]
    
    return result, records


def format_us(us: int) -> str:
    """Format microseconds as human-readable."""
    if us >= 1_000_000:
        return f"{us/1_000_000:.2f}s"
    elif us >= 1000:
        return f"{us/1000:.1f}ms"
    else:
        return f"{us}us"


def print_scorecard(result: ScorecardResult, verbose: bool = False):
    """Print scorecard to stdout."""
    grade = result.grade()
    
    print("=" * 60)
    print("V1G2 SIMPLE STABILITY SCORECARD")
    print("=" * 60)
    print()
    
    # Summary
    print(f"Log Stats: {result.total_lines} lines, {result.perf_records} perf records, "
          f"{result.steady_state_records} steady-state")
    if result.parse_errors > 0:
        print(f"  ⚠️  Parse errors: {result.parse_errors}")
    if result.reconnect_detected:
        print(f"  ℹ️  Reconnect detected at millis={result.reconnect_millis}")
    print()
    
    # Invariants
    print("INVARIANTS:")
    inv_pass = result.invariants_pass()
    print(f"  qDrop:     {result.total_q_drop:5d}  {'✅ PASS' if result.total_q_drop == 0 else '❌ FAIL'}")
    print(f"  parseFail: {result.total_parse_fail:5d}  {'✅ PASS' if result.total_parse_fail == 0 else '❌ FAIL'}")
    print(f"  disc:      {result.total_disc:5d}  {'✅ PASS' if result.total_disc == 0 else '❌ FAIL'}")
    print(f"  reconn:    {result.max_reconn:5d}  ℹ️  (informational)")
    print()
    
    # SLOs
    print("SLO METRICS (steady-state):")
    slo_checks = [
        ("loopMax_us", result.max_loop_us, SLO_LOOP_MAX_US),
        ("bleDrainMax_us", result.max_ble_drain_us, SLO_BLE_DRAIN_MAX_US),
        ("wifiMax_us", result.max_wifi_us, SLO_WIFI_MAX_US),
        ("fsMax_us", result.max_fs_us, SLO_FS_MAX_US),
        ("sdMax_us", result.max_sd_us, SLO_SD_MAX_US),
    ]
    for name, value, threshold in slo_checks:
        status = "✅ PASS" if value <= threshold else "❌ FAIL"
        print(f"  {name:20s} {format_us(value):>10s}  (limit: {format_us(threshold):>8s})  {status}")
    print()
    
    # BLE connection metrics (informational)
    print("BLE CONNECTION METRICS (informational):")
    print(f"  bleConnMax_us:     {format_us(result.max_ble_conn_us):>10s}")
    print(f"  bleDiscMax_us:     {format_us(result.max_ble_disc_us):>10s}")
    print(f"  bleSubsMax_us:     {format_us(result.max_ble_subs_us):>10s}")
    print()
    
    # Mystery stalls
    if result.mystery_stalls > 0:
        print(f"⚠️  MYSTERY STALLS: {result.mystery_stalls} (loopMax exceeded but no attribution)")
        print()
    
    # Worst samples
    if result.worst_samples:
        print("TOP 5 WORST SAMPLES:")
        for i, sample in enumerate(result.worst_samples, 1):
            attr = sample.find_attribution()
            attr_str = f" → {attr}" if attr else " → UNATTRIBUTED"
            print(f"  {i}. millis={sample.millis:6d}  loopMax={format_us(sample.loop_max_us):>10s}{attr_str}")
            if verbose:
                # Show all attribution metrics
                for name, value in sample.get_attribution_metrics().items():
                    if value > 0:
                        pct = value / sample.loop_max_us * 100 if sample.loop_max_us > 0 else 0
                        print(f"       {name}: {format_us(value)} ({pct:.0f}%)")
        print()
    
    # Final grade
    print("=" * 60)
    grade_emoji = {"GREEN": "🟢", "YELLOW": "🟡", "RED": "🔴"}.get(grade, "❓")
    print(f"GRADE: {grade_emoji} {grade}")
    print("=" * 60)
    
    # Violations summary
    violations = result.slo_violations()
    if violations:
        print()
        print("SLO Violations:")
        for v in violations:
            print(f"  - {v}")


def main():
    parser = argparse.ArgumentParser(
        description="V1G2 Simple Stability Scorecard",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("logfile", help="Path to NDJSON debug log file")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show detailed attribution")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()
    
    try:
        result, records = parse_log(args.logfile)
    except FileNotFoundError:
        print(f"ERROR: File not found: {args.logfile}", file=sys.stderr)
        return 3
    except Exception as e:
        print(f"ERROR: Failed to parse log: {e}", file=sys.stderr)
        return 3
    
    if result.perf_records == 0:
        print("ERROR: No perf records found in log", file=sys.stderr)
        return 3
    
    if args.json:
        output = {
            "grade": result.grade(),
            "invariants_pass": result.invariants_pass(),
            "slo_violations": result.slo_violations(),
            "mystery_stalls": result.mystery_stalls,
            "stats": {
                "total_lines": result.total_lines,
                "perf_records": result.perf_records,
                "steady_state_records": result.steady_state_records,
                "parse_errors": result.parse_errors,
            },
            "invariants": {
                "qDrop": result.total_q_drop,
                "parseFail": result.total_parse_fail,
                "disc": result.total_disc,
                "reconn": result.max_reconn,
            },
            "slo_max": {
                "loopMax_us": result.max_loop_us,
                "bleDrainMax_us": result.max_ble_drain_us,
                "wifiMax_us": result.max_wifi_us,
                "fsMax_us": result.max_fs_us,
                "sdMax_us": result.max_sd_us,
            },
            "ble_connection": {
                "bleConnMax_us": result.max_ble_conn_us,
                "bleDiscMax_us": result.max_ble_disc_us,
                "bleSubsMax_us": result.max_ble_subs_us,
            },
            "reconnect_detected": result.reconnect_detected,
        }
        print(json.dumps(output, indent=2))
    else:
        print_scorecard(result, verbose=args.verbose)
    
    # Exit code based on grade
    grade = result.grade()
    if grade == "GREEN":
        return 0
    elif grade == "YELLOW":
        return 1
    else:
        return 2


if __name__ == "__main__":
    sys.exit(main())
