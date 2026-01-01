#!/usr/bin/env python3
"""
Parse V1 Gen2 metrics output from serial log.
Extracts [METRICS] lines and computes statistics.

Usage:
    python parse_metrics.py <serial_log.txt> [--json] [--csv]
    cat serial.log | python parse_metrics.py - [--json]
"""

import sys
import re
import json
import argparse
from collections import defaultdict
from dataclasses import dataclass, field
from typing import List, Dict, Optional

@dataclass
class MetricsSample:
    """Single metrics sample from [METRICS] line"""
    timestamp_ms: int = 0
    rx_packets: int = 0
    parse_count: int = 0
    drop_count: int = 0
    high_water: int = 0
    latency_min_us: int = 0
    latency_avg_us: int = 0
    latency_max_us: int = 0
    display_updates: int = 0

@dataclass
class MetricsStats:
    """Computed statistics from multiple samples"""
    sample_count: int = 0
    rx_packets_total: int = 0
    parse_total: int = 0
    drop_total: int = 0
    high_water_max: int = 0
    latency_min_us: int = 999999999
    latency_max_us: int = 0
    latency_sum_us: int = 0
    latency_samples: int = 0
    display_updates_total: int = 0
    
    # Thresholds for pass/fail
    MAX_LATENCY_US: int = 100000  # 100ms
    MAX_DROP_RATE: float = 0.01   # 1%
    MAX_HIGH_WATER: int = 32
    
    def add_sample(self, sample: MetricsSample):
        self.sample_count += 1
        self.rx_packets_total = sample.rx_packets  # Cumulative in firmware
        self.parse_total = sample.parse_count
        self.drop_total = sample.drop_count
        self.high_water_max = max(self.high_water_max, sample.high_water)
        
        if sample.latency_avg_us > 0:
            self.latency_min_us = min(self.latency_min_us, sample.latency_min_us)
            self.latency_max_us = max(self.latency_max_us, sample.latency_max_us)
            self.latency_sum_us += sample.latency_avg_us
            self.latency_samples += 1
        
        self.display_updates_total = sample.display_updates
    
    def latency_avg_us(self) -> int:
        return self.latency_sum_us // self.latency_samples if self.latency_samples > 0 else 0
    
    def drop_rate(self) -> float:
        total = self.rx_packets_total + self.drop_total
        return self.drop_total / total if total > 0 else 0.0
    
    def check_thresholds(self) -> Dict[str, bool]:
        """Check all thresholds, return dict of pass/fail"""
        return {
            'latency_max': self.latency_max_us <= self.MAX_LATENCY_US,
            'drop_rate': self.drop_rate() <= self.MAX_DROP_RATE,
            'high_water': self.high_water_max <= self.MAX_HIGH_WATER,
        }
    
    def all_passed(self) -> bool:
        return all(self.check_thresholds().values())
    
    def to_dict(self) -> Dict:
        return {
            'samples': self.sample_count,
            'rx_packets': self.rx_packets_total,
            'parse_count': self.parse_total,
            'drops': self.drop_total,
            'drop_rate': f'{self.drop_rate()*100:.2f}%',
            'high_water_max': self.high_water_max,
            'latency_min_us': self.latency_min_us if self.latency_min_us < 999999999 else 0,
            'latency_avg_us': self.latency_avg_us(),
            'latency_max_us': self.latency_max_us,
            'display_updates': self.display_updates_total,
            'thresholds': self.check_thresholds(),
            'passed': self.all_passed(),
        }


def parse_metrics_line(line: str) -> Optional[MetricsSample]:
    """Parse a [METRICS] line into a MetricsSample"""
    # Format: [METRICS] BLE→DRAW: min=X avg=Y max=Z ms (n=N)
    # Format: [METRICS] Queue: overflow=X drop=Y | Parse=Z | Display=W skip=V
    # Format: [METRICS] rx=X parse=Y drop=Z hw=W lat=MIN/AVG/MAXus updates=U
    
    sample = MetricsSample()
    
    # Try compact format first
    compact = re.search(r'rx=(\d+)\s+parse=(\d+)\s+drop=(\d+)\s+hw=(\d+)\s+lat=(\d+)/(\d+)/(\d+)us\s+updates=(\d+)', line)
    if compact:
        sample.rx_packets = int(compact.group(1))
        sample.parse_count = int(compact.group(2))
        sample.drop_count = int(compact.group(3))
        sample.high_water = int(compact.group(4))
        sample.latency_min_us = int(compact.group(5))
        sample.latency_avg_us = int(compact.group(6))
        sample.latency_max_us = int(compact.group(7))
        sample.display_updates = int(compact.group(8))
        return sample
    
    # Try BLE→DRAW format
    ble_draw = re.search(r'BLE→DRAW:\s*min=(\d+)\s+avg=(\d+)\s+max=(\d+)\s+ms', line)
    if ble_draw:
        sample.latency_min_us = int(ble_draw.group(1)) * 1000
        sample.latency_avg_us = int(ble_draw.group(2)) * 1000
        sample.latency_max_us = int(ble_draw.group(3)) * 1000
        return sample
    
    # Try Queue format
    queue = re.search(r'overflow=(\d+)\s+drop=(\d+)', line)
    if queue:
        sample.drop_count = int(queue.group(1)) + int(queue.group(2))
        
    parse = re.search(r'Parse=(\d+)', line)
    if parse:
        sample.parse_count = int(parse.group(1))
        
    display = re.search(r'Display=(\d+)', line)
    if display:
        sample.display_updates = int(display.group(1))
    
    if queue or parse or display:
        return sample
    
    return None


def parse_log(input_stream) -> MetricsStats:
    """Parse entire log and return computed stats"""
    stats = MetricsStats()
    
    for line in input_stream:
        line = line.strip()
        if '[METRICS]' not in line:
            continue
        
        sample = parse_metrics_line(line)
        if sample:
            stats.add_sample(sample)
    
    return stats


def main():
    parser = argparse.ArgumentParser(description='Parse V1 Gen2 metrics from serial log')
    parser.add_argument('logfile', help='Log file to parse (use - for stdin)')
    parser.add_argument('--json', action='store_true', help='Output as JSON')
    parser.add_argument('--csv', action='store_true', help='Output as CSV')
    parser.add_argument('--max-latency', type=int, default=100000, 
                        help='Max latency threshold in microseconds (default: 100000)')
    parser.add_argument('--max-drops', type=float, default=0.01,
                        help='Max drop rate threshold (default: 0.01 = 1%%)')
    args = parser.parse_args()
    
    # Open input
    if args.logfile == '-':
        input_stream = sys.stdin
    else:
        input_stream = open(args.logfile, 'r')
    
    try:
        stats = parse_log(input_stream)
    finally:
        if args.logfile != '-':
            input_stream.close()
    
    # Apply custom thresholds
    stats.MAX_LATENCY_US = args.max_latency
    stats.MAX_DROP_RATE = args.max_drops
    
    # Output
    result = stats.to_dict()
    
    if args.json:
        print(json.dumps(result, indent=2))
    elif args.csv:
        headers = ['samples', 'rx_packets', 'parse_count', 'drops', 'drop_rate', 
                   'high_water_max', 'latency_min_us', 'latency_avg_us', 'latency_max_us',
                   'display_updates', 'passed']
        print(','.join(headers))
        values = [str(result.get(h, '')) for h in headers]
        print(','.join(values))
    else:
        # Human-readable summary
        print("=" * 50)
        print("V1 Gen2 Performance Metrics Summary")
        print("=" * 50)
        print(f"Samples collected:     {result['samples']}")
        print(f"RX packets:            {result['rx_packets']}")
        print(f"Parse count:           {result['parse_count']}")
        print(f"Drops:                 {result['drops']} ({result['drop_rate']})")
        print(f"Queue high water:      {result['high_water_max']}")
        print(f"Latency (BLE→flush):   {result['latency_min_us']}/{result['latency_avg_us']}/{result['latency_max_us']} µs")
        print(f"Display updates:       {result['display_updates']}")
        print("-" * 50)
        print("Threshold checks:")
        for name, passed in result['thresholds'].items():
            status = "✓ PASS" if passed else "✗ FAIL"
            print(f"  {name}: {status}")
        print("-" * 50)
        print(f"Overall: {'✓ ALL PASSED' if result['passed'] else '✗ FAILED'}")
        print("=" * 50)
    
    # Exit code based on pass/fail
    sys.exit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
