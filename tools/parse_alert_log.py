#!/usr/bin/env python3
"""
Parse debug logs to extract alert sequences for testing.

Works with pre-hex logs that have [Alerts] entries like:
[    179712 ms] [Alerts] count=1 pri=Ka dir=Rear freq=35497 front=5 rear=5 muted=false

Usage:
    python tools/parse_alert_log.py debug.log --stats
    python tools/parse_alert_log.py debug.log --segments
    python tools/parse_alert_log.py debug.log --extract-ka --output fixtures/
    python tools/parse_alert_log.py debug.log --extract-multi --output fixtures/
"""

import argparse
import json
import re
import sys
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import Optional


@dataclass
class AlertEntry:
    """Single alert log entry."""
    timestamp_ms: int
    count: int
    priority_band: str
    direction: str
    frequency: int
    front_signal: int
    rear_signal: int
    muted: bool
    
    @property
    def frequency_ghz(self) -> float:
        """Convert MHz*1000 to GHz."""
        return self.frequency / 1000.0


@dataclass
class AlertSegment:
    """A continuous segment of alerts (gap < threshold means same segment)."""
    start_ms: int
    end_ms: int
    entries: list
    peak_signal: int = 0
    priority_band: str = ""
    frequencies: set = None
    max_bogeys: int = 0
    
    def __post_init__(self):
        if self.frequencies is None:
            self.frequencies = set()
        if self.entries:
            self._analyze()
    
    def _analyze(self):
        """Analyze entries to extract stats."""
        for entry in self.entries:
            sig = max(entry.front_signal, entry.rear_signal)
            if sig > self.peak_signal:
                self.peak_signal = sig
            self.frequencies.add(entry.frequency)
            if entry.count > self.max_bogeys:
                self.max_bogeys = entry.count
            # Priority is from first entry
            if not self.priority_band:
                self.priority_band = entry.priority_band
    
    @property
    def duration_ms(self) -> int:
        return self.end_ms - self.start_ms
    
    def to_dict(self) -> dict:
        return {
            "start_ms": self.start_ms,
            "end_ms": self.end_ms,
            "duration_ms": self.duration_ms,
            "priority_band": self.priority_band,
            "peak_signal": self.peak_signal,
            "max_bogeys": self.max_bogeys,
            "frequencies_mhz": sorted([f/1000.0 for f in self.frequencies]),
            "entry_count": len(self.entries),
        }


def parse_alert_line(line: str) -> Optional[AlertEntry]:
    """Parse a single [Alerts] log line."""
    # Pattern: [    179712 ms] [Alerts] count=1 pri=Ka dir=Rear freq=35497 front=5 rear=5 muted=false
    pattern = r'\[\s*(\d+)\s*ms\]\s*\[Alerts\]\s*count=(\d+)\s+pri=(\w+)\s+dir=(\w+)\s+freq=(\d+)\s+front=(\d+)\s+rear=(\d+)\s+muted=(\w+)'
    
    match = re.search(pattern, line)
    if not match:
        return None
    
    return AlertEntry(
        timestamp_ms=int(match.group(1)),
        count=int(match.group(2)),
        priority_band=match.group(3),
        direction=match.group(4),
        frequency=int(match.group(5)),
        front_signal=int(match.group(6)),
        rear_signal=int(match.group(7)),
        muted=match.group(8).lower() == 'true'
    )


def parse_log_file(filepath: str) -> list[AlertEntry]:
    """Parse all alert entries from a log file."""
    entries = []
    with open(filepath, 'r') as f:
        for line in f:
            if '[Alerts]' in line:
                entry = parse_alert_line(line)
                if entry:
                    entries.append(entry)
    # Sort by timestamp to handle log files with device restarts
    entries.sort(key=lambda e: e.timestamp_ms)
    return entries


def segment_alerts(entries: list[AlertEntry], gap_threshold_ms: int = 2000) -> list[AlertSegment]:
    """Group alerts into segments based on time gaps."""
    if not entries:
        return []
    
    segments = []
    current_entries = [entries[0]]
    
    for i in range(1, len(entries)):
        gap = entries[i].timestamp_ms - entries[i-1].timestamp_ms
        if gap > gap_threshold_ms:
            # Start new segment
            seg = AlertSegment(
                start_ms=current_entries[0].timestamp_ms,
                end_ms=current_entries[-1].timestamp_ms,
                entries=current_entries
            )
            segments.append(seg)
            current_entries = []
        current_entries.append(entries[i])
    
    # Don't forget last segment
    if current_entries:
        seg = AlertSegment(
            start_ms=current_entries[0].timestamp_ms,
            end_ms=current_entries[-1].timestamp_ms,
            entries=current_entries
        )
        segments.append(seg)
    
    return segments


def print_stats(entries: list[AlertEntry], segments: list[AlertSegment]):
    """Print statistics about the log."""
    if not entries:
        print("No alerts found in log.")
        return
    
    duration_ms = entries[-1].timestamp_ms - entries[0].timestamp_ms
    duration_min = duration_ms / 60000
    
    print(f"\n{'='*60}")
    print(f"ALERT LOG STATISTICS")
    print(f"{'='*60}")
    print(f"Total entries:     {len(entries)}")
    print(f"Session duration:  {duration_min:.1f} minutes ({duration_ms} ms)")
    print(f"Alert segments:    {len(segments)}")
    
    # Band breakdown
    bands = {}
    for e in entries:
        bands[e.priority_band] = bands.get(e.priority_band, 0) + 1
    
    print(f"\nBy Band:")
    for band, count in sorted(bands.items(), key=lambda x: -x[1]):
        print(f"  {band:8s}: {count:5d} entries")
    
    # Frequency breakdown
    freqs = {}
    for e in entries:
        f = e.frequency / 1000.0
        key = f"{f:.3f}"
        freqs[key] = freqs.get(key, 0) + 1
    
    print(f"\nTop Frequencies (MHz):")
    for freq, count in sorted(freqs.items(), key=lambda x: -x[1])[:10]:
        print(f"  {freq:>10s}: {count:5d} entries")
    
    # Bogey counts
    bogeys = {}
    for e in entries:
        bogeys[e.count] = bogeys.get(e.count, 0) + 1
    
    print(f"\nBy Bogey Count:")
    for bc, count in sorted(bogeys.items()):
        print(f"  {bc} bogeys: {count:5d} entries")
    
    # Segment analysis
    if segments:
        durations = [s.duration_ms for s in segments]
        print(f"\nSegment Durations:")
        print(f"  Min:     {min(durations):6d} ms")
        print(f"  Max:     {max(durations):6d} ms")
        print(f"  Average: {sum(durations)/len(durations):6.0f} ms")
        
        # Multi-alert segments
        multi = [s for s in segments if s.max_bogeys > 1]
        ka_segs = [s for s in segments if s.priority_band == 'Ka']
        print(f"\n  Ka segments:         {len(ka_segs)}")
        print(f"  Multi-alert segments: {len(multi)}")
    
    print(f"{'='*60}\n")


def print_segments(segments: list[AlertSegment]):
    """Print segment summary."""
    print(f"\n{'#':>4} {'Start':>10} {'Dur(ms)':>8} {'Band':>4} {'Peak':>4} {'Bogeys':>6} {'Freqs'}")
    print("-" * 70)
    
    for i, seg in enumerate(segments):
        freqs_str = ", ".join([f"{f:.1f}" for f in sorted(seg.frequencies)[:3]])
        if len(seg.frequencies) > 3:
            freqs_str += f" (+{len(seg.frequencies)-3})"
        print(f"{i+1:4d} {seg.start_ms:10d} {seg.duration_ms:8d} {seg.priority_band:>4} {seg.peak_signal:4d} {seg.max_bogeys:6d} {freqs_str}")


def export_segment_fixture(segment: AlertSegment, index: int, output_dir: Path, prefix: str):
    """Export a segment as a JSON fixture."""
    fixture = {
        "metadata": {
            "source": "debug_log",
            "segment_index": index,
            "band": segment.priority_band,
            "duration_ms": segment.duration_ms,
            "peak_signal": segment.peak_signal,
            "max_bogeys": segment.max_bogeys,
        },
        "alerts": []
    }
    
    base_ts = segment.entries[0].timestamp_ms
    for entry in segment.entries:
        fixture["alerts"].append({
            "ts_offset_ms": entry.timestamp_ms - base_ts,
            "count": entry.count,
            "band": entry.priority_band,
            "direction": entry.direction,
            "frequency_mhz": entry.frequency / 1000.0,
            "front_signal": entry.front_signal,
            "rear_signal": entry.rear_signal,
            "muted": entry.muted,
        })
    
    filename = f"{prefix}_seg{index:03d}_{segment.priority_band}_{segment.duration_ms}ms.json"
    filepath = output_dir / filename
    
    with open(filepath, 'w') as f:
        json.dump(fixture, f, indent=2)
    
    print(f"  Wrote: {filepath.name}")
    return filepath


def main():
    parser = argparse.ArgumentParser(description="Parse V1G2 debug logs for alert data")
    parser.add_argument("logfile", help="Path to debug log file")
    parser.add_argument("--stats", action="store_true", help="Print statistics")
    parser.add_argument("--segments", action="store_true", help="List all segments")
    parser.add_argument("--extract-ka", action="store_true", help="Extract Ka band segments to fixtures")
    parser.add_argument("--extract-multi", action="store_true", help="Extract multi-alert segments to fixtures")
    parser.add_argument("--extract-all", action="store_true", help="Extract all segments to fixtures")
    parser.add_argument("--output", "-o", type=str, default=".", help="Output directory for fixtures")
    parser.add_argument("--gap", type=int, default=2000, help="Gap threshold (ms) to split segments")
    parser.add_argument("--min-duration", type=int, default=500, help="Minimum segment duration (ms) to export")
    
    args = parser.parse_args()
    
    if not Path(args.logfile).exists():
        print(f"Error: File not found: {args.logfile}", file=sys.stderr)
        sys.exit(1)
    
    print(f"Parsing: {args.logfile}")
    entries = parse_log_file(args.logfile)
    print(f"Found {len(entries)} alert entries")
    
    segments = segment_alerts(entries, args.gap)
    print(f"Grouped into {len(segments)} segments")
    
    if args.stats or (not any([args.segments, args.extract_ka, args.extract_multi, args.extract_all])):
        print_stats(entries, segments)
    
    if args.segments:
        print_segments(segments)
    
    # Export fixtures
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = Path(args.logfile).stem
    
    exported = 0
    
    if args.extract_ka:
        print(f"\nExporting Ka band segments to {output_dir}/")
        for i, seg in enumerate(segments):
            if seg.priority_band == 'Ka' and seg.duration_ms >= args.min_duration:
                export_segment_fixture(seg, i, output_dir, prefix)
                exported += 1
    
    if args.extract_multi:
        print(f"\nExporting multi-alert segments to {output_dir}/")
        for i, seg in enumerate(segments):
            if seg.max_bogeys > 1 and seg.duration_ms >= args.min_duration:
                export_segment_fixture(seg, i, output_dir, prefix)
                exported += 1
    
    if args.extract_all:
        print(f"\nExporting all segments to {output_dir}/")
        for i, seg in enumerate(segments):
            if seg.duration_ms >= args.min_duration:
                export_segment_fixture(seg, i, output_dir, prefix)
                exported += 1
    
    if exported:
        print(f"\nExported {exported} fixtures")


if __name__ == "__main__":
    main()
