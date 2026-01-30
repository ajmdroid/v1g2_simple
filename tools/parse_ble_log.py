#!/usr/bin/env python3
"""
Parse V1 Simple debug logs and extract BLE packets for replay testing.

Usage:
    python tools/parse_ble_log.py debug.log -o test/fixtures/drive_capture.json
    
Log format expected:
    [     12345 ms] [BLE:PKT] ts=12345 len=19 hex=AA040A430C...

Output JSON format:
    {
        "metadata": {
            "source": "debug.log",
            "captured": "2026-01-30T12:00:00Z",
            "packet_count": 1234,
            "duration_ms": 300000
        },
        "packets": [
            {"ts": 12345, "len": 19, "hex": "AA040A430C..."},
            ...
        ]
    }
"""

import argparse
import json
import re
import sys
from datetime import datetime
from pathlib import Path


# Regex to match BLE packet log lines
# Format: [     12345 ms] [BLE:PKT] ts=12345 len=19 hex=AA040A430C...
PACKET_RE = re.compile(
    r'\[\s*(\d+)\s*ms\]\s*\[BLE:PKT\]\s*ts=(\d+)\s+len=(\d+)\s+hex=([0-9A-Fa-f]+)'
)

# Regex to match alert state changes (for correlation)
# Format: [     12345 ms] [Alerts] count=1 pri=Ka dir=FRONT freq=34712 ...
ALERT_RE = re.compile(
    r'\[\s*(\d+)\s*ms\]\s*\[Alerts\]\s*count=(\d+)\s+pri=(\w+)\s+dir=(\w+)\s+freq=(\d+)'
)


def parse_log_file(log_path: Path) -> dict:
    """Parse a debug log file and extract BLE packets."""
    packets = []
    alerts = []  # For correlation/validation
    
    with open(log_path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            # Try to match BLE packet line
            match = PACKET_RE.search(line)
            if match:
                log_ts = int(match.group(1))  # Timestamp from log prefix
                pkt_ts = int(match.group(2))  # Timestamp from packet data
                length = int(match.group(3))
                hex_data = match.group(4).upper()
                
                # Validate hex length matches declared length
                if len(hex_data) != length * 2:
                    print(f"WARNING: Hex length mismatch at ts={pkt_ts}: "
                          f"declared={length}, actual={len(hex_data)//2}", file=sys.stderr)
                    continue
                
                packets.append({
                    "ts": pkt_ts,
                    "len": length,
                    "hex": hex_data
                })
                continue
            
            # Try to match alert state line (for correlation)
            match = ALERT_RE.search(line)
            if match:
                alerts.append({
                    "ts": int(match.group(1)),
                    "count": int(match.group(2)),
                    "band": match.group(3),
                    "dir": match.group(4),
                    "freq": int(match.group(5))
                })
    
    return {"packets": packets, "alerts": alerts}


def analyze_packets(packets: list) -> dict:
    """Analyze packet statistics."""
    if not packets:
        return {"count": 0}
    
    # Calculate timing
    first_ts = packets[0]["ts"]
    last_ts = packets[-1]["ts"]
    duration_ms = last_ts - first_ts
    
    # Count packet types (packet ID is at offset 3, hex chars 6-7)
    type_counts = {}
    for pkt in packets:
        if len(pkt["hex"]) >= 8:
            pkt_id = pkt["hex"][6:8]
            type_counts[pkt_id] = type_counts.get(pkt_id, 0) + 1
    
    # Decode packet types
    PKT_NAMES = {
        "31": "DisplayData",
        "43": "AlertData", 
        "63": "Voltage",
        "41": "RespAlertData",
        "42": "SweepSection",
        "61": "UserBytes"
    }
    
    type_summary = {}
    for pkt_id, count in sorted(type_counts.items()):
        name = PKT_NAMES.get(pkt_id, f"Unknown(0x{pkt_id})")
        type_summary[name] = count
    
    return {
        "count": len(packets),
        "duration_ms": duration_ms,
        "duration_str": f"{duration_ms // 60000}m {(duration_ms // 1000) % 60}s",
        "first_ts": first_ts,
        "last_ts": last_ts,
        "packet_types": type_summary,
        "packets_per_second": len(packets) / max(1, duration_ms / 1000)
    }


def find_interesting_segments(packets: list, alerts: list) -> list:
    """Find interesting time windows with alerts."""
    segments = []
    
    if not alerts:
        return segments
    
    # Group consecutive alerts into segments
    current_segment = None
    
    for alert in alerts:
        if alert["count"] > 0:  # Alert active
            if current_segment is None:
                current_segment = {
                    "start_ts": alert["ts"],
                    "end_ts": alert["ts"],
                    "max_count": alert["count"],
                    "bands": {alert["band"]},
                    "freqs": {alert["freq"]}
                }
            else:
                current_segment["end_ts"] = alert["ts"]
                current_segment["max_count"] = max(current_segment["max_count"], alert["count"])
                current_segment["bands"].add(alert["band"])
                current_segment["freqs"].add(alert["freq"])
        else:  # Alert cleared
            if current_segment is not None:
                current_segment["end_ts"] = alert["ts"]
                # Convert sets to lists for JSON
                current_segment["bands"] = list(current_segment["bands"])
                current_segment["freqs"] = list(current_segment["freqs"])
                segments.append(current_segment)
                current_segment = None
    
    # Handle unclosed segment
    if current_segment is not None:
        current_segment["bands"] = list(current_segment["bands"])
        current_segment["freqs"] = list(current_segment["freqs"])
        segments.append(current_segment)
    
    return segments


def extract_segment_packets(packets: list, start_ts: int, end_ts: int, 
                            margin_ms: int = 1000) -> list:
    """Extract packets within a time window (with margin)."""
    return [
        pkt for pkt in packets
        if (start_ts - margin_ms) <= pkt["ts"] <= (end_ts + margin_ms)
    ]


def main():
    parser = argparse.ArgumentParser(
        description="Parse V1 Simple debug logs and extract BLE packets for replay"
    )
    parser.add_argument("log_file", type=Path, help="Path to debug.log file")
    parser.add_argument("-o", "--output", type=Path, 
                        help="Output JSON file (default: stdout)")
    parser.add_argument("--stats", action="store_true",
                        help="Print statistics only, don't output packets")
    parser.add_argument("--segments", action="store_true",
                        help="Find and list interesting alert segments")
    parser.add_argument("--extract-segment", type=int, metavar="N",
                        help="Extract packets for segment N (0-indexed)")
    parser.add_argument("--margin", type=int, default=2000,
                        help="Time margin (ms) around segments (default: 2000)")
    
    args = parser.parse_args()
    
    if not args.log_file.exists():
        print(f"ERROR: Log file not found: {args.log_file}", file=sys.stderr)
        sys.exit(1)
    
    print(f"Parsing {args.log_file}...", file=sys.stderr)
    data = parse_log_file(args.log_file)
    
    packets = data["packets"]
    alerts = data["alerts"]
    
    stats = analyze_packets(packets)
    
    print(f"\n=== Packet Statistics ===", file=sys.stderr)
    print(f"Total packets: {stats['count']}", file=sys.stderr)
    if stats['count'] > 0:
        print(f"Duration: {stats['duration_str']}", file=sys.stderr)
        print(f"Rate: {stats['packets_per_second']:.1f} packets/sec", file=sys.stderr)
        print(f"\nPacket types:", file=sys.stderr)
        for pkt_type, count in stats.get('packet_types', {}).items():
            print(f"  {pkt_type}: {count}", file=sys.stderr)
    
    if alerts:
        print(f"\nAlert state changes logged: {len(alerts)}", file=sys.stderr)
    
    if args.segments or args.extract_segment is not None:
        segments = find_interesting_segments(packets, alerts)
        
        if args.segments:
            print(f"\n=== Interesting Segments ({len(segments)}) ===", file=sys.stderr)
            for i, seg in enumerate(segments):
                duration = seg["end_ts"] - seg["start_ts"]
                print(f"  [{i}] ts={seg['start_ts']}-{seg['end_ts']} "
                      f"({duration}ms) max_bogeys={seg['max_count']} "
                      f"bands={seg['bands']} freqs={seg['freqs']}", file=sys.stderr)
        
        if args.extract_segment is not None:
            if 0 <= args.extract_segment < len(segments):
                seg = segments[args.extract_segment]
                seg_packets = extract_segment_packets(
                    packets, seg["start_ts"], seg["end_ts"], args.margin
                )
                packets = seg_packets
                print(f"\nExtracted {len(packets)} packets from segment {args.extract_segment}",
                      file=sys.stderr)
            else:
                print(f"ERROR: Segment {args.extract_segment} not found "
                      f"(0-{len(segments)-1} available)", file=sys.stderr)
                sys.exit(1)
    
    if args.stats:
        sys.exit(0)
    
    # Build output JSON
    output = {
        "metadata": {
            "source": str(args.log_file.name),
            "captured": datetime.now().isoformat(),
            "packet_count": len(packets),
            "stats": stats
        },
        "packets": packets
    }
    
    # Output
    json_str = json.dumps(output, indent=2)
    
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json_str)
        print(f"\nWrote {len(packets)} packets to {args.output}", file=sys.stderr)
    else:
        print(json_str)


if __name__ == "__main__":
    main()
