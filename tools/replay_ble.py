#!/usr/bin/env python3
"""
BLE Packet Replay Tool for V1 Simple

Parses debug logs, extracts BLE packets, and can replay them over serial
to test the firmware with real captured data.

Usage:
    # Extract and analyze a log file
    python tools/replay_ble.py debug-3.log --analyze
    
    # Extract packets around alerts
    python tools/replay_ble.py debug-3.log --extract-alerts -o alert_packets.json
    
    # Replay packets over serial
    python tools/replay_ble.py alert_packets.json --replay --port /dev/tty.usbserial-XXX
    
    # Replay with speed multiplier (2x = faster)
    python tools/replay_ble.py alert_packets.json --replay --port /dev/tty.usbserial-XXX --speed 2.0
"""

import argparse
import json
import re
import sys
import time
from datetime import datetime
from pathlib import Path
from collections import defaultdict

# Packet type IDs (from V1 protocol)
PKT_TYPES = {
    0x31: "DisplayData",
    0x43: "AlertData",
    0x63: "Voltage",
    0x41: "RespAlertData",
    0x42: "SweepSection",
    0x61: "UserBytes",
}

# Band codes from packet data
BANDS = {
    0x01: "Laser",
    0x02: "Ka",
    0x04: "K",
    0x08: "X",
    0x10: "Ku",
}

# Direction codes
DIRECTIONS = {
    0x01: "Front",
    0x02: "Side",
    0x04: "Rear",
}


def parse_ndjson_log(log_path: Path) -> dict:
    """Parse debug log (NDJSON or legacy plain-text format)."""
    packets = []
    alerts = []
    
    # Regex to extract packet data from message
    pkt_re = re.compile(r'\[BLE:PKT\]\s*ts=(\d+)\s+len=(\d+)\s+hex=([0-9A-Fa-f]+)')
    alert_re = re.compile(r'\[Alerts\]\s*count=(\d+)\s+pri=(\w+)\s+dir=(\w+)\s+freq=(\d+)')
    
    # Legacy format with timestamp prefix
    legacy_ts_re = re.compile(r'\[\s*(\d+)\s*ms\]')
    
    with open(log_path, 'r', encoding='utf-8', errors='replace') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            
            millis = None
            message = line
            
            # Try NDJSON format first
            if line.startswith('{'):
                try:
                    entry = json.loads(line)
                    millis = entry.get("millis", 0)
                    message = entry.get("message", "")
                except json.JSONDecodeError:
                    pass
            else:
                # Legacy format - extract timestamp from prefix
                ts_match = legacy_ts_re.match(line)
                if ts_match:
                    millis = int(ts_match.group(1))
            
            # Check for BLE packet
            match = pkt_re.search(message)
            if match:
                pkt_ts = int(match.group(1))
                length = int(match.group(2))
                hex_data = match.group(3).upper()
                
                if len(hex_data) == length * 2:
                    packets.append({
                        "ts": pkt_ts,
                        "len": length,
                        "hex": hex_data
                    })
                continue
            
            # Check for alert state
            match = alert_re.search(message)
            if match:
                alerts.append({
                    "ts": millis if millis else 0,
                    "count": int(match.group(1)),
                    "band": match.group(2),
                    "dir": match.group(3),
                    "freq": int(match.group(4))
                })
    
    return {"packets": packets, "alerts": alerts}


def deduplicate_packets(packets: list) -> list:
    """Remove duplicate packets at same timestamp."""
    seen = set()
    unique = []
    
    for pkt in packets:
        key = (pkt["ts"], pkt["hex"])
        if key not in seen:
            seen.add(key)
            unique.append(pkt)
    
    return sorted(unique, key=lambda p: p["ts"])


def decode_alert_packet(hex_data: str) -> dict:
    """Decode an AlertData packet (0x43)."""
    data = bytes.fromhex(hex_data)
    
    # AlertData format:
    # Byte 0: Start delimiter (0xAA)
    # Byte 1-2: Destination/Source
    # Byte 3: Packet ID (0x43)
    # Byte 4: Payload length
    # Payload (starting at byte 5):
    #   Byte 0: high nibble = alert index, low nibble = alert count
    #   Byte 1-2: Frequency MSB/LSB (MHz for K/X, 100 MHz units for Ka)
    #   Byte 3: Front RSSI
    #   Byte 4: Rear RSSI
    #   Byte 5: bandArrow (band in lower 4 bits + direction in upper bits + mute)
    #   Byte 6: aux0 (bit 7 = isPriority)
    # Last 2: Checksum, End delimiter (0xAB)
    
    if len(data) < 13:  # Minimum alert packet length
        return None
    
    pkt_id = data[3]
    if pkt_id != 0x43:
        return None
    
    # Payload starts at byte 5
    payload = data[5:-2]  # Exclude checksum and end delimiter
    if len(payload) < 7:
        return None
    
    try:
        alert_idx = (payload[0] >> 4) & 0x0F
        alert_count = payload[0] & 0x0F
        
        if alert_count == 0:
            return {"count": 0}
        
        freq_raw = (payload[1] << 8) | payload[2]
        front_rssi = payload[3]
        rear_rssi = payload[4]
        band_arrow = payload[5]
        aux0 = payload[6]
        
        # Decode band from lower bits
        if band_arrow & 0x01:
            band = "Laser"
        elif band_arrow & 0x02:
            band = "Ka"
        elif band_arrow & 0x04:
            band = "K"
        elif band_arrow & 0x08:
            band = "X"
        else:
            band = "Unknown"
        
        # Decode direction from upper bits
        if band_arrow & 0x20:
            direction = "Front"
        elif band_arrow & 0x40:
            direction = "Side"
        elif band_arrow & 0x80:
            direction = "Rear"
        else:
            direction = "Unknown"
        
        # Convert frequency based on band
        # K-band: value is MHz (e.g., 24123 = 24.123 GHz)
        # Ka-band: value is 100 MHz units (e.g., 34712 = 34.712 GHz)
        if band == "K" or band == "X":
            freq_ghz = freq_raw / 1000.0
        elif band == "Ka":
            freq_ghz = freq_raw / 1000.0  # Ka frequencies already in MHz
        else:
            freq_ghz = freq_raw
        
        is_muted = (band_arrow & 0x10) != 0
        is_priority = (aux0 & 0x80) != 0
        
        return {
            "index": alert_idx,
            "count": alert_count,
            "freq_ghz": freq_ghz,
            "freq_raw": freq_raw,
            "front_rssi": front_rssi,
            "rear_rssi": rear_rssi,
            "band": band,
            "direction": direction,
            "muted": is_muted,
            "priority": is_priority
        }
    except IndexError:
        return None


def analyze_packets(packets: list) -> dict:
    """Analyze packet statistics."""
    if not packets:
        return {
            "total_packets": 0,
            "duration_ms": 0,
            "duration_str": "0m 0s",
            "packets_per_second": 0,
            "packet_types": {},
            "alert_frequencies": {}
        }
    
    packets = deduplicate_packets(packets)
    
    # Calculate timing
    first_ts = packets[0]["ts"]
    last_ts = packets[-1]["ts"]
    duration_ms = last_ts - first_ts
    
    # Count packet types
    type_counts = defaultdict(int)
    alert_freqs = defaultdict(int)
    
    for pkt in packets:
        if len(pkt["hex"]) >= 8:
            pkt_id = int(pkt["hex"][6:8], 16)
            type_name = PKT_TYPES.get(pkt_id, f"Unknown(0x{pkt_id:02X})")
            type_counts[type_name] += 1
            
            # Decode alert packets
            if pkt_id == 0x43:
                alert = decode_alert_packet(pkt["hex"])
                if alert and alert.get("count", 0) > 0:
                    key = f"{alert['band']} @ {alert['freq_ghz']:.3f} GHz ({alert['direction']})"
                    alert_freqs[key] += 1
    
    return {
        "total_packets": len(packets),
        "duration_ms": duration_ms,
        "duration_str": f"{duration_ms // 60000}m {(duration_ms // 1000) % 60}s",
        "packets_per_second": len(packets) / max(1, duration_ms / 1000),
        "packet_types": dict(type_counts),
        "alert_frequencies": dict(alert_freqs)
    }


def find_alert_windows(packets: list, alerts: list, margin_ms: int = 5000) -> list:
    """Find time windows around alert activity."""
    if not alerts:
        return []
    
    # Sort alerts by timestamp
    alerts = sorted(alerts, key=lambda a: a["ts"])
    
    # Group alerts into segments (gaps > margin_ms = new segment)
    segments = []
    current = None
    
    for alert in alerts:
        if current is None:
            current = {
                "start_ts": alert["ts"],
                "end_ts": alert["ts"],
                "bands": {alert["band"]},
                "max_count": alert["count"],
                "freqs": {alert["freq"]}
            }
        elif alert["ts"] - current["end_ts"] > margin_ms:
            # Gap too large, start new segment
            segments.append(current)
            current = {
                "start_ts": alert["ts"],
                "end_ts": alert["ts"],
                "bands": {alert["band"]},
                "max_count": alert["count"],
                "freqs": {alert["freq"]}
            }
        else:
            # Extend current segment
            current["end_ts"] = alert["ts"]
            current["bands"].add(alert["band"])
            current["max_count"] = max(current["max_count"], alert["count"])
            current["freqs"].add(alert["freq"])
    
    if current:
        segments.append(current)
    
    # Convert sets to lists for JSON
    for seg in segments:
        seg["bands"] = list(seg["bands"])
        seg["freqs"] = list(seg["freqs"])
        seg["duration_ms"] = seg["end_ts"] - seg["start_ts"]
    
    return segments


def extract_segment_packets(packets: list, start_ts: int, end_ts: int, 
                            margin_ms: int = 2000) -> list:
    """Extract packets within a time window (with margin)."""
    result = []
    for pkt in packets:
        if (start_ts - margin_ms) <= pkt["ts"] <= (end_ts + margin_ms):
            result.append(pkt)
    return result


def normalize_timestamps(packets: list) -> list:
    """Normalize timestamps to start at 0."""
    if not packets:
        return []
    
    min_ts = min(p["ts"] for p in packets)
    return [{"ts": p["ts"] - min_ts, "len": p["len"], "hex": p["hex"]} 
            for p in packets]


def replay_packets(packets: list, port: str, speed: float = 1.0, dry_run: bool = False):
    """Replay packets over serial with timing."""
    import serial
    
    if dry_run:
        print("DRY RUN - would send packets:")
        for i, pkt in enumerate(packets[:10]):
            print(f"  [{i}] ts={pkt['ts']}ms len={pkt['len']} hex={pkt['hex'][:20]}...")
        if len(packets) > 10:
            print(f"  ... and {len(packets) - 10} more packets")
        return
    
    print(f"Opening {port}...")
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(2)  # Wait for device reset
    
    print(f"Replaying {len(packets)} packets at {speed}x speed...")
    
    start_time = time.time()
    last_ts = 0
    
    try:
        for i, pkt in enumerate(packets):
            # Calculate delay
            delay_ms = (pkt["ts"] - last_ts) / speed
            if delay_ms > 0:
                time.sleep(delay_ms / 1000.0)
            
            # Send packet as hex line
            data = bytes.fromhex(pkt["hex"])
            ser.write(data)
            
            last_ts = pkt["ts"]
            
            if i % 100 == 0:
                elapsed = time.time() - start_time
                print(f"  Sent {i}/{len(packets)} packets ({elapsed:.1f}s elapsed)")
    
    except KeyboardInterrupt:
        print("\nReplay interrupted")
    finally:
        ser.close()
    
    elapsed = time.time() - start_time
    print(f"Replay complete: {len(packets)} packets in {elapsed:.1f}s")


def main():
    parser = argparse.ArgumentParser(
        description="BLE Packet Replay Tool for V1 Simple"
    )
    parser.add_argument("input", type=Path, help="Input log file or JSON packet file")
    parser.add_argument("-o", "--output", type=Path, help="Output JSON file")
    
    # Analysis options
    parser.add_argument("--analyze", action="store_true",
                        help="Analyze and show statistics")
    parser.add_argument("--segments", action="store_true",
                        help="Find and list alert segments")
    parser.add_argument("--extract-segment", type=int, metavar="N",
                        help="Extract packets for segment N (0-indexed)")
    parser.add_argument("--extract-alerts", action="store_true",
                        help="Extract all packets around alert windows")
    parser.add_argument("--margin", type=int, default=3000,
                        help="Time margin (ms) around alerts (default: 3000)")
    
    # Replay options
    parser.add_argument("--replay", action="store_true",
                        help="Replay packets over serial")
    parser.add_argument("--port", type=str, help="Serial port for replay")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="Replay speed multiplier (default: 1.0)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Don't actually send, just show what would be sent")
    
    args = parser.parse_args()
    
    if not args.input.exists():
        print(f"ERROR: Input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)
    
    # Determine input type
    if args.input.suffix == ".json":
        print(f"Loading JSON packet file: {args.input}", file=sys.stderr)
        with open(args.input) as f:
            data = json.load(f)
        packets = data.get("packets", [])
        alerts = data.get("alerts", [])
    else:
        print(f"Parsing log file: {args.input}", file=sys.stderr)
        data = parse_ndjson_log(args.input)
        packets = data["packets"]
        alerts = data["alerts"]
    
    # Deduplicate
    packets = deduplicate_packets(packets)
    print(f"Loaded {len(packets)} unique packets, {len(alerts)} alert states", file=sys.stderr)
    
    # Analysis mode
    if args.analyze:
        stats = analyze_packets(packets)
        print("\n=== Packet Statistics ===")
        print(f"Total unique packets: {stats['total_packets']}")
        print(f"Duration: {stats['duration_str']}")
        print(f"Rate: {stats['packets_per_second']:.1f} packets/sec")
        print("\nPacket types:")
        for pkt_type, count in sorted(stats['packet_types'].items()):
            print(f"  {pkt_type}: {count}")
        if stats['alert_frequencies']:
            print("\nAlert frequencies detected:")
            for freq, count in sorted(stats['alert_frequencies'].items(), key=lambda x: -x[1]):
                print(f"  {freq}: {count} packets")
    
    # Find segments
    if args.segments or args.extract_segment is not None or args.extract_alerts:
        segments = find_alert_windows(packets, alerts, args.margin)
        
        if args.segments:
            print(f"\n=== Alert Segments ({len(segments)}) ===")
            for i, seg in enumerate(segments):
                print(f"  [{i}] ts={seg['start_ts']}-{seg['end_ts']} "
                      f"({seg['duration_ms']}ms) max_bogeys={seg['max_count']} "
                      f"bands={seg['bands']}")
        
        # Extract specific segment
        if args.extract_segment is not None:
            if 0 <= args.extract_segment < len(segments):
                seg = segments[args.extract_segment]
                packets = extract_segment_packets(
                    packets, seg["start_ts"], seg["end_ts"], args.margin
                )
                packets = normalize_timestamps(packets)
                print(f"\nExtracted {len(packets)} packets from segment {args.extract_segment}",
                      file=sys.stderr)
            else:
                print(f"ERROR: Segment {args.extract_segment} not found "
                      f"(0-{len(segments)-1} available)", file=sys.stderr)
                sys.exit(1)
        
        # Extract all alert packets
        if args.extract_alerts:
            all_alert_packets = []
            for seg in segments:
                seg_pkts = extract_segment_packets(
                    packets, seg["start_ts"], seg["end_ts"], args.margin
                )
                all_alert_packets.extend(seg_pkts)
            
            # Dedupe again and normalize
            packets = deduplicate_packets(all_alert_packets)
            packets = normalize_timestamps(packets)
            print(f"\nExtracted {len(packets)} packets from {len(segments)} alert windows",
                  file=sys.stderr)
    
    # Replay mode
    if args.replay:
        if not args.port and not args.dry_run:
            print("ERROR: --port required for replay (or use --dry-run)", file=sys.stderr)
            sys.exit(1)
        replay_packets(packets, args.port, args.speed, args.dry_run)
    
    # Output JSON
    if args.output:
        output = {
            "metadata": {
                "source": str(args.input.name),
                "extracted": datetime.now().isoformat(),
                "packet_count": len(packets),
            },
            "packets": packets,
            "alerts": alerts if not args.extract_segment and not args.extract_alerts else []
        }
        
        with open(args.output, 'w') as f:
            json.dump(output, f, indent=2)
        print(f"\nWrote {len(packets)} packets to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
