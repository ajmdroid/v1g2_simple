#!/usr/bin/env python3
"""Analyze drive session logs for correlations across log categories."""

import re
import sys
from collections import defaultdict
import statistics

def analyze_log(log_file):
    # Data structures
    rssi_values = []
    gps_fixes = []
    alerts = []
    display_times = defaultdict(list)
    lockouts = []
    auto_lockouts = []
    
    with open(log_file, 'r') as f:
        for line in f:
            # Extract timestamp
            ts_match = re.match(r'\[\s*(\d+)\s*ms\]', line)
            if not ts_match:
                continue
            ts = int(ts_match.group(1))
            
            # RSSI
            rssi_match = re.search(r'V1 RSSI: (-?\d+) dBm', line)
            if rssi_match:
                rssi_values.append((ts, int(rssi_match.group(1))))
            
            # GPS Fix
            gps_match = re.search(r'FIX:.*HDOP:\s*([\d.]+).*Sats:\s*(\d+)', line)
            if gps_match:
                gps_fixes.append((ts, float(gps_match.group(1)), int(gps_match.group(2))))
            
            # Alerts: [Alerts] count=1 pri=K dir=Front freq=24180 front=1 rear=0 muted=true
            alert_match = re.search(r'\[Alerts\]\s+count=(\d+)\s+pri=(\w+)\s+dir=(\w+)\s+freq=(\d+)\s+front=(\d+)\s+rear=(\d+)', line)
            if alert_match:
                count = int(alert_match.group(1))
                band = alert_match.group(2)
                direction = alert_match.group(3)
                freq = int(alert_match.group(4))
                front_sig = int(alert_match.group(5))
                rear_sig = int(alert_match.group(6))
                muted = 'muted=true' in line
                alerts.append((ts, band, direction, freq, front_sig, rear_sig, muted, line.strip()))
            
            # Display timing
            display_match = re.search(r'\[SLOW\]\s*([\w.()]+):\s*(\d+)ms', line)
            if display_match:
                display_times[display_match.group(1)].append((ts, int(display_match.group(2))))
            
            # Lockout activity
            if 'lockout' in line.lower() or 'Lockout' in line:
                lockouts.append((ts, line.strip()))
            
            # Auto-lockout specific
            if 'cluster' in line.lower() or 'AutoLockout' in line:
                auto_lockouts.append((ts, line.strip()))

    print("=" * 70)
    print("                    DRIVE SESSION ANALYSIS")
    print("=" * 70)

    # Time range
    all_times = [r[0] for r in rssi_values] + [g[0] for g in gps_fixes]
    if all_times:
        start_ms = min(all_times)
        end_ms = max(all_times)
        duration_min = (end_ms - start_ms) / 60000
        print(f"\n⏱️  Drive Duration: {duration_min:.1f} minutes ({(end_ms-start_ms)/1000:.0f}s)")

    # BLE/RSSI Analysis
    print(f"\n📶 BLE RSSI Analysis ({len(rssi_values)} samples)")
    if rssi_values:
        rssi_only = [r[1] for r in rssi_values]
        print(f"   Range: {min(rssi_only)} to {max(rssi_only)} dBm")
        print(f"   Average: {statistics.mean(rssi_only):.1f} dBm")
        print(f"   Std Dev: {statistics.stdev(rssi_only):.1f} dBm")
        
        # Find RSSI drops (potential BLE issues)
        drops = [(rssi_values[i][0], rssi_values[i-1][1], rssi_values[i][1]) 
                 for i in range(1, len(rssi_values)) 
                 if rssi_values[i][1] - rssi_values[i-1][1] < -10]
        if drops:
            print(f"   ⚠️  Significant drops (>10dB): {len(drops)}")

    # GPS Analysis
    print(f"\n🛰️  GPS Analysis ({len(gps_fixes)} fixes)")
    if gps_fixes:
        hdops = [g[1] for g in gps_fixes]
        sats = [g[2] for g in gps_fixes]
        print(f"   HDOP: {min(hdops):.1f} - {max(hdops):.1f} (avg: {statistics.mean(hdops):.2f})")
        print(f"   Satellites: {min(sats)} - {max(sats)} (avg: {statistics.mean(sats):.1f})")
        first_fix_ms = gps_fixes[0][0]
        print(f"   Time to first fix: {first_fix_ms/1000:.1f}s")
        
        # GPS quality over time
        mid = len(gps_fixes) // 2
        first_half_hdop = statistics.mean([g[1] for g in gps_fixes[:mid]])
        second_half_hdop = statistics.mean([g[1] for g in gps_fixes[mid:]])
        print(f"   HDOP trend: {first_half_hdop:.2f} → {second_half_hdop:.2f}")

    # Display Performance
    print(f"\n🖥️  Display Performance")
    total_slow = 0
    for mode, times in sorted(display_times.items()):
        durations = [t[1] for t in times]
        total_slow += len(times)
        avg = statistics.mean(durations)
        mx = max(durations)
        print(f"   {mode}: {len(times):4d} samples, avg={avg:5.1f}ms, max={mx:3d}ms")
    print(f"   Total slow frames: {total_slow}")

    # Alert patterns
    print(f"\n🚨 Alert Analysis ({len(alerts)} total)")
    band_counts = defaultdict(int)
    band_freqs = defaultdict(list)
    muted_count = 0
    direction_counts = defaultdict(int)
    
    for a in alerts:
        ts, band, direction, freq, front_sig, rear_sig, muted, _ = a
        band_counts[band] += 1
        band_freqs[band].append(freq)
        direction_counts[direction] += 1
        if muted:
            muted_count += 1
    
    for band in ['K', 'Ka', 'X', 'Laser']:
        if band in band_counts:
            count = band_counts[band]
            freqs = band_freqs[band]
            if freqs:
                unique_freqs = len(set(freqs))
                freq_range = f"{min(freqs)}-{max(freqs)} MHz"
                print(f"   {band:5s}: {count:5d} alerts, {unique_freqs} unique freqs ({freq_range})")
            else:
                print(f"   {band:5s}: {count:5d} alerts")
    
    print(f"\n   Directions: {dict(direction_counts)}")
    print(f"   Muted alerts: {muted_count} ({100*muted_count/len(alerts) if alerts else 0:.1f}%)")

    # Find alert clusters (alerts within 2 seconds of each other)
    if alerts:
        clusters = []
        current_cluster = [alerts[0]]
        for i in range(1, len(alerts)):
            if alerts[i][0] - alerts[i-1][0] < 2000:
                current_cluster.append(alerts[i])
            else:
                if len(current_cluster) > 5:  # Significant cluster
                    clusters.append(current_cluster)
                current_cluster = [alerts[i]]
        if len(current_cluster) > 5:
            clusters.append(current_cluster)
        
        print(f"\n   Alert clusters (>5 alerts within 2s gaps): {len(clusters)}")
        if clusters:
            for i, cluster in enumerate(clusters[:5]):  # Show first 5
                duration = (cluster[-1][0] - cluster[0][0]) / 1000
                bands = set(a[1] for a in cluster)
                freqs = set(a[3] for a in cluster)
                print(f"     #{i+1}: {len(cluster)} alerts over {duration:.1f}s, bands: {', '.join(bands)}, freqs: {freqs}")

    # Lockout activity
    print(f"\n🔒 Lockout Activity")
    print(f"   Total lockout log entries: {len(lockouts)}")
    print(f"   Auto-lockout entries: {len(auto_lockouts)}")

    # CORRELATIONS
    print(f"\n" + "=" * 70)
    print("                        CORRELATIONS")
    print("=" * 70)

    # 1. RSSI during alerts vs idle
    if rssi_values and alerts:
        alert_windows = set()
        for a in alerts:
            for t in range(a[0] - 1000, a[0] + 1000, 100):
                alert_windows.add(t // 1000)
        
        rssi_during_alerts = [r[1] for r in rssi_values if r[0] // 1000 in alert_windows]
        rssi_idle = [r[1] for r in rssi_values if r[0] // 1000 not in alert_windows]
        
        if rssi_during_alerts and rssi_idle:
            alert_avg = statistics.mean(rssi_during_alerts)
            idle_avg = statistics.mean(rssi_idle)
            print(f"\n📶 RSSI vs Alert State:")
            print(f"   During alerts: {alert_avg:.1f} dBm ({len(rssi_during_alerts)} samples)")
            print(f"   During idle:   {idle_avg:.1f} dBm ({len(rssi_idle)} samples)")
            diff = alert_avg - idle_avg
            if abs(diff) > 2:
                print(f"   ⚠️  {abs(diff):.1f}dB difference - {'better' if diff > 0 else 'worse'} during alerts")
            else:
                print(f"   ✅ No significant difference ({diff:+.1f}dB)")

    # 2. GPS quality during alerts
    if gps_fixes and alerts:
        alert_windows = set()
        for a in alerts:
            for t in range(a[0] - 2000, a[0] + 2000, 100):
                alert_windows.add(t // 1000)
        
        gps_during_alerts = [g[1] for g in gps_fixes if g[0] // 1000 in alert_windows]
        gps_idle = [g[1] for g in gps_fixes if g[0] // 1000 not in alert_windows]
        
        if gps_during_alerts and gps_idle:
            print(f"\n🛰️  GPS HDOP vs Alert State:")
            print(f"   During alerts: {statistics.mean(gps_during_alerts):.2f} ({len(gps_during_alerts)} samples)")
            print(f"   During idle:   {statistics.mean(gps_idle):.2f} ({len(gps_idle)} samples)")

    # 3. Display performance vs alert state
    if display_times and alerts:
        alert_display = display_times.get('display.update(alerts)', [])
        rest_display = display_times.get('display.resting', [])
        persist_display = display_times.get('display.persisted', [])
        
        print(f"\n🖥️  Display Mode Distribution:")
        print(f"   Alert mode frames:     {len(alert_display):5d} ({sum(d[1] for d in alert_display)/1000:.1f}s total render time)")
        print(f"   Resting mode frames:   {len(rest_display):5d} ({sum(d[1] for d in rest_display)/1000:.1f}s total render time)")
        print(f"   Persisted mode frames: {len(persist_display):5d} ({sum(d[1] for d in persist_display)/1000:.1f}s total render time)")
        
        # Display latency after alert starts
        if alert_display and alerts:
            # Find first display update after each alert cluster starts
            latencies = []
            for cluster in clusters:
                cluster_start = cluster[0][0]
                first_display = next((d for d in alert_display if d[0] >= cluster_start), None)
                if first_display:
                    latencies.append(first_display[0] - cluster_start)
            if latencies:
                print(f"   Alert→Display latency: avg={statistics.mean(latencies):.0f}ms, max={max(latencies)}ms")

    # 4. Alert density over time
    if alerts:
        print(f"\n📊 Alert Density Over Time:")
        # Split drive into quarters
        if len(alerts) > 10:
            quarter = len(alerts) // 4
            q1 = alerts[:quarter]
            q2 = alerts[quarter:2*quarter]
            q3 = alerts[2*quarter:3*quarter]
            q4 = alerts[3*quarter:]
            
            for i, q in enumerate([q1, q2, q3, q4], 1):
                if q:
                    duration = (q[-1][0] - q[0][0]) / 60000  # minutes
                    rate = len(q) / duration if duration > 0 else 0
                    print(f"   Q{i}: {len(q):5d} alerts, {rate:.1f} alerts/min")

    print("\n" + "=" * 70)


if __name__ == '__main__':
    log_file = sys.argv[1] if len(sys.argv) > 1 else "/Users/ajmedford/v1g2_simple/test/fixtures/drive_session.log"
    analyze_log(log_file)
