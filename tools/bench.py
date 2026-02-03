#!/usr/bin/env python3
"""
Benchmark log analyzer for V1 Gen2 Simple Display.

Parses NDJSON debug logs and extracts performance metrics, incidents,
and session summaries. Designed for post-drive analysis.

Usage:
    python tools/bench.py <logfile>
    python tools/bench.py debug-14.log

Output:
    - Session summary (duration, boot segments)
    - Per-segment performance metrics
    - Incident detection and analysis
    - Anomaly identification (stalls, drops, heap pressure)
"""

import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class PerfSample:
    """A single perf metrics sample from the log."""
    timestamp: str  # ISO8601
    uptime_ms: int
    rx: int = 0
    qDrop: int = 0
    qHW: int = 0
    loopMax_us: int = 0
    heapMin: int = 0
    blockMin: int = 0
    wifiMax_us: int = 0
    fsMax_us: int = 0
    sdMax_us: int = 0
    flushMax_us: int = 0
    bleDrainMax_us: int = 0
    dispMax_ms: int = 0
    prxMax_ms: int = 0
    disc: int = 0
    reconn: int = 0


@dataclass
class BootSegment:
    """A continuous session starting from a BOOT line."""
    boot_id: str
    git_sha: str
    scenario: str
    log_format: str
    start_time: str  # ISO8601
    samples: list = field(default_factory=list)
    incidents: list = field(default_factory=list)
    
    @property
    def duration_ms(self) -> int:
        if len(self.samples) < 2:
            return 0
        return self.samples[-1].uptime_ms - self.samples[0].uptime_ms
    
    @property
    def duration_str(self) -> str:
        ms = self.duration_ms
        s = ms // 1000
        m, s = divmod(s, 60)
        h, m = divmod(m, 60)
        if h > 0:
            return f"{h}h{m}m{s}s"
        elif m > 0:
            return f"{m}m{s}s"
        else:
            return f"{s}s"


@dataclass
class Incident:
    """A detected anomaly or triggered investigation."""
    timestamp: str
    uptime_ms: int
    incident_type: str  # "loopStall", "queueDrop", "heapPressure", "wifiStall"
    details: dict


def parse_ndjson_log(filepath: str) -> tuple[list[BootSegment], dict]:
    """Parse an NDJSON debug log file into boot segments and summary stats.
    
    Handles malformed/truncated files gracefully - always produces a summary.
    """
    
    segments: list[BootSegment] = []
    current_segment: Optional[BootSegment] = None
    line_count = 0
    parse_errors = 0
    text_lines = 0  # Non-NDJSON lines (legacy format)
    truncated_lines = 0  # Lines that look truncated (no closing brace)
    
    # Thresholds for anomaly detection
    STALL_THRESHOLD_US = 500000  # 500ms
    HEAP_PRESSURE_THRESHOLD = 50000  # 50KB free heap
    WIFI_STALL_THRESHOLD_US = 1000000  # 1s
    
    try:
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                line_count += 1
                line = line.strip()
                if not line:
                    continue
                
                # Try to parse as NDJSON
                if line.startswith('{'):
                    # Check for truncated JSON (no closing brace)
                    if not line.rstrip().endswith('}'):
                        truncated_lines += 1
                        continue
                    
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError as e:
                        parse_errors += 1
                        continue
                
                cat = obj.get('cat', '')
                ts = obj.get('ts', '')
                msg = obj.get('msg', '')
                
                # Handle BOOT lines - start new segment
                if cat == 'system' and msg.startswith('BOOT'):
                    boot_id = obj.get('bootId', f'unknown-{len(segments)}')
                    git_sha = obj.get('git', 'unknown')
                    scenario = obj.get('scenario', 'unknown')
                    log_format = obj.get('logFormat', 'unknown')
                    
                    current_segment = BootSegment(
                        boot_id=boot_id,
                        git_sha=git_sha,
                        scenario=scenario,
                        log_format=log_format,
                        start_time=ts
                    )
                    segments.append(current_segment)
                    continue
                
                # Handle perf samples
                if cat == 'perf' and current_segment:
                    sample = PerfSample(
                        timestamp=ts,
                        uptime_ms=obj.get('uptime_ms', 0),
                        rx=obj.get('rx', 0),
                        qDrop=obj.get('qDrop', 0),
                        qHW=obj.get('qHW', 0),
                        loopMax_us=obj.get('loopMax_us', 0),
                        heapMin=obj.get('heapMin', 0),
                        blockMin=obj.get('blockMin', 0),
                        wifiMax_us=obj.get('wifiMax_us', 0),
                        fsMax_us=obj.get('fsMax_us', 0),
                        sdMax_us=obj.get('sdMax_us', 0),
                        flushMax_us=obj.get('flushMax_us', 0),
                        bleDrainMax_us=obj.get('bleDrainMax_us', 0),
                        dispMax_ms=obj.get('dispMax_ms', 0),
                        prxMax_ms=obj.get('prxMax_ms', 0),
                        disc=obj.get('disc', 0),
                        reconn=obj.get('reconn', 0),
                    )
                    current_segment.samples.append(sample)
                    
                    # Detect anomalies
                    if sample.loopMax_us > STALL_THRESHOLD_US:
                        current_segment.incidents.append(Incident(
                            timestamp=ts,
                            uptime_ms=sample.uptime_ms,
                            incident_type='loopStall',
                            details={
                                'loopMax_us': sample.loopMax_us,
                                'wifiMax_us': sample.wifiMax_us,
                                'fsMax_us': sample.fsMax_us,
                                'sdMax_us': sample.sdMax_us,
                            }
                        ))
                    
                    if sample.heapMin < HEAP_PRESSURE_THRESHOLD and sample.heapMin > 0:
                        current_segment.incidents.append(Incident(
                            timestamp=ts,
                            uptime_ms=sample.uptime_ms,
                            incident_type='heapPressure',
                            details={
                                'heapMin': sample.heapMin,
                                'blockMin': sample.blockMin,
                            }
                        ))
                    
                    if sample.wifiMax_us > WIFI_STALL_THRESHOLD_US:
                        current_segment.incidents.append(Incident(
                            timestamp=ts,
                            uptime_ms=sample.uptime_ms,
                            incident_type='wifiStall',
                            details={
                                'wifiMax_us': sample.wifiMax_us,
                                'fsMax_us': sample.fsMax_us,
                            }
                        ))
                
                # Handle investigation_triggered messages
                if cat == 'system' and 'investigation_triggered' in msg and current_segment:
                    # Parse: "investigation_triggered loopMax_us=123 qDropDelta=0"
                    loop_match = re.search(r'loopMax_us=(\d+)', msg)
                    qdrop_match = re.search(r'qDropDelta=(\d+)', msg)
                    current_segment.incidents.append(Incident(
                        timestamp=ts,
                        uptime_ms=obj.get('uptime_ms', 0),
                        incident_type='triggered',
                        details={
                            'loopMax_us': int(loop_match.group(1)) if loop_match else 0,
                            'qDropDelta': int(qdrop_match.group(1)) if qdrop_match else 0,
                        }
                    ))
            else:
                # Non-JSON line (legacy text format or other)
                text_lines += 1
                
                # Try to detect BOOT in text format: "[...] BOOT ..."
                if 'BOOT' in line and current_segment is None:
                    # Create placeholder segment for text-format logs
                    current_segment = BootSegment(
                        boot_id=f'text-{len(segments)}',
                        git_sha='unknown',
                        scenario='text',
                        log_format='text',
                        start_time='unknown'
                    )
                    segments.append(current_segment)
    except Exception as e:
        # Catch any unexpected errors (corrupted file, I/O issues, etc.)
        # Still return partial results
        parse_errors += 1
    
    summary = {
        'total_lines': line_count,
        'parse_errors': parse_errors,
        'truncated_lines': truncated_lines,
        'text_lines': text_lines,
        'ndjson_lines': line_count - text_lines - parse_errors - truncated_lines,
    }
    
    return segments, summary


def analyze_segment(seg: BootSegment) -> dict:
    """Compute aggregate statistics for a boot segment."""
    if not seg.samples:
        return {}
    
    samples = seg.samples
    
    # Delta calculations (first to last sample)
    first, last = samples[0], samples[-1]
    rx_delta = last.rx - first.rx
    qdrop_delta = last.qDrop - first.qDrop
    disc_delta = last.disc - first.disc
    reconn_delta = last.reconn - first.reconn
    
    # Max values across all samples
    max_loop_us = max(s.loopMax_us for s in samples)
    max_wifi_us = max(s.wifiMax_us for s in samples)
    max_fs_us = max(s.fsMax_us for s in samples)
    max_sd_us = max(s.sdMax_us for s in samples)
    max_flush_us = max(s.flushMax_us for s in samples)
    max_drain_us = max(s.bleDrainMax_us for s in samples)
    
    # Min heap/block (worst case)
    min_heap = min((s.heapMin for s in samples if s.heapMin > 0), default=0)
    min_block = min((s.blockMin for s in samples if s.blockMin > 0), default=0)
    
    # Average calculations
    avg_loop_us = sum(s.loopMax_us for s in samples) / len(samples)
    
    # Count stalls (samples with loopMax > 100ms)
    stall_count_100ms = sum(1 for s in samples if s.loopMax_us > 100000)
    stall_count_500ms = sum(1 for s in samples if s.loopMax_us > 500000)
    stall_count_1s = sum(1 for s in samples if s.loopMax_us > 1000000)
    
    return {
        'duration_ms': seg.duration_ms,
        'sample_count': len(samples),
        'rx_total': last.rx,
        'rx_delta': rx_delta,
        'qdrop_delta': qdrop_delta,
        'disc_delta': disc_delta,
        'reconn_delta': reconn_delta,
        'max_loop_us': max_loop_us,
        'max_loop_ms': max_loop_us / 1000,
        'max_wifi_us': max_wifi_us,
        'max_fs_us': max_fs_us,
        'max_sd_us': max_sd_us,
        'max_flush_us': max_flush_us,
        'max_drain_us': max_drain_us,
        'min_heap': min_heap,
        'min_block': min_block,
        'avg_loop_us': avg_loop_us,
        'stalls_100ms': stall_count_100ms,
        'stalls_500ms': stall_count_500ms,
        'stalls_1s': stall_count_1s,
        'incidents': len(seg.incidents),
    }


def format_us(us: int) -> str:
    """Format microseconds as human-readable."""
    if us >= 1000000:
        return f"{us/1000000:.2f}s"
    elif us >= 1000:
        return f"{us/1000:.1f}ms"
    else:
        return f"{us}µs"


def format_bytes(b: int) -> str:
    """Format bytes as human-readable."""
    if b >= 1048576:
        return f"{b/1048576:.1f}MB"
    elif b >= 1024:
        return f"{b/1024:.1f}KB"
    else:
        return f"{b}B"


def print_report(filepath: str, segments: list[BootSegment], summary: dict):
    """Print a human-readable analysis report."""
    
    print("=" * 72)
    print("               V1G2 BENCHMARK LOG ANALYSIS REPORT")
    print("=" * 72)
    print(f"\nFile: {filepath}")
    print(f"Lines: {summary['total_lines']:,} total, {summary['ndjson_lines']:,} NDJSON, "
          f"{summary['text_lines']:,} text, {summary['parse_errors']} errors, "
          f"{summary.get('truncated_lines', 0)} truncated")
    print(f"Boot segments: {len(segments)}")
    
    if not segments:
        print("\n⚠️  No boot segments found - is this a valid debug log?")
        return
    
    # Per-segment analysis
    for i, seg in enumerate(segments, 1):
        stats = analyze_segment(seg)
        if not stats:
            continue
        
        print(f"\n{'─' * 72}")
        print(f"SEGMENT {i}: {seg.boot_id}")
        print(f"{'─' * 72}")
        print(f"  Git: {seg.git_sha[:12] if len(seg.git_sha) >= 12 else seg.git_sha}")
        print(f"  Scenario: {seg.scenario}, Format: {seg.log_format}")
        print(f"  Start: {seg.start_time}")
        print(f"  Duration: {seg.duration_str} ({stats['sample_count']} perf samples)")
        
        print(f"\n  📦 Packets:")
        print(f"     RX total: {stats['rx_total']:,}, delta: {stats['rx_delta']:,}")
        print(f"     Queue drops: {stats['qdrop_delta']}, disconnects: {stats['disc_delta']}, reconnects: {stats['reconn_delta']}")
        
        print(f"\n  ⏱️  Timing (max per 5s window):")
        print(f"     Loop:  {format_us(stats['max_loop_us']):>10}  (avg: {format_us(int(stats['avg_loop_us']))})")
        print(f"     WiFi:  {format_us(stats['max_wifi_us']):>10}")
        print(f"     FS:    {format_us(stats['max_fs_us']):>10}")
        print(f"     SD:    {format_us(stats['max_sd_us']):>10}")
        print(f"     Flush: {format_us(stats['max_flush_us']):>10}")
        print(f"     BLE:   {format_us(stats['max_drain_us']):>10}")
        
        print(f"\n  💾 Memory:")
        print(f"     Min heap: {format_bytes(stats['min_heap'])}")
        print(f"     Min block: {format_bytes(stats['min_block'])}")
        
        print(f"\n  ⚠️  Stalls:")
        print(f"     >100ms: {stats['stalls_100ms']}, >500ms: {stats['stalls_500ms']}, >1s: {stats['stalls_1s']}")
        
        # Show incidents
        if seg.incidents:
            print(f"\n  🔴 Incidents ({len(seg.incidents)}):")
            for inc in seg.incidents[:10]:  # Show first 10
                if inc.incident_type == 'loopStall':
                    print(f"     [{inc.timestamp}] STALL: loop={format_us(inc.details['loopMax_us'])}, "
                          f"wifi={format_us(inc.details.get('wifiMax_us', 0))}, "
                          f"fs={format_us(inc.details.get('fsMax_us', 0))}")
                elif inc.incident_type == 'wifiStall':
                    print(f"     [{inc.timestamp}] WIFI: {format_us(inc.details['wifiMax_us'])}, "
                          f"fs={format_us(inc.details.get('fsMax_us', 0))}")
                elif inc.incident_type == 'heapPressure':
                    print(f"     [{inc.timestamp}] HEAP: {format_bytes(inc.details['heapMin'])}, "
                          f"block={format_bytes(inc.details.get('blockMin', 0))}")
                elif inc.incident_type == 'triggered':
                    print(f"     [{inc.timestamp}] TRIGGERED: loop={format_us(inc.details.get('loopMax_us', 0))}, "
                          f"qDrop={inc.details.get('qDropDelta', 0)}")
            if len(seg.incidents) > 10:
                print(f"     ... and {len(seg.incidents) - 10} more")
    
    # Overall summary
    print(f"\n{'=' * 72}")
    print("                          OVERALL SUMMARY")
    print("=" * 72)
    
    total_duration_ms = sum(analyze_segment(s).get('duration_ms', 0) for s in segments)
    total_rx = sum(analyze_segment(s).get('rx_delta', 0) for s in segments)
    total_drops = sum(analyze_segment(s).get('qdrop_delta', 0) for s in segments)
    total_stalls_500ms = sum(analyze_segment(s).get('stalls_500ms', 0) for s in segments)
    total_incidents = sum(len(s.incidents) for s in segments)
    
    total_h = total_duration_ms // 3600000
    total_m = (total_duration_ms % 3600000) // 60000
    
    print(f"  Total runtime: {total_h}h {total_m}m")
    print(f"  Total RX packets: {total_rx:,}")
    print(f"  Total queue drops: {total_drops}")
    print(f"  Total stalls (>500ms): {total_stalls_500ms}")
    print(f"  Total incidents: {total_incidents}")
    
    # Find worst stall across all segments
    worst_stall = 0
    worst_stall_seg = None
    for seg in segments:
        for s in seg.samples:
            if s.loopMax_us > worst_stall:
                worst_stall = s.loopMax_us
                worst_stall_seg = seg
    
    if worst_stall > 0 and worst_stall_seg:
        print(f"\n  🔴 Worst stall: {format_us(worst_stall)} in segment {worst_stall_seg.boot_id}")
    
    print()


def main():
    if len(sys.argv) < 2:
        print("Usage: python tools/bench.py <logfile>")
        print("       python tools/bench.py debug-14.log")
        sys.exit(1)
    
    filepath = sys.argv[1]
    if not Path(filepath).exists():
        print(f"Error: File not found: {filepath}")
        sys.exit(1)
    
    segments, summary = parse_ndjson_log(filepath)
    print_report(filepath, segments, summary)


if __name__ == '__main__':
    main()
