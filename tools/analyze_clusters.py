#!/usr/bin/env python3
"""
Analyze auto-lockout cluster data from debug logs.
"""

import re
import math
import sys

def haversine(lat1, lon1, lat2, lon2):
    """Calculate distance between two GPS coordinates in meters."""
    R = 6371000
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlam/2)**2
    return 2 * R * math.asin(math.sqrt(a))


def analyze_clusters(logfile):
    """Parse and analyze cluster data from log file."""
    
    # Parse cluster creations
    clusters = {}
    with open(logfile, 'r') as f:
        for line in f:
            m = re.search(r'NEW CLUSTER @ \(([0-9.-]+),([0-9.-]+)\)', line)
            if m:
                cluster_id = len(clusters)
                lat, lon = float(m.group(1)), float(m.group(2))
                freq_m = re.search(r'K ([0-9.]+)MHz', line)
                freq = freq_m.group(1) if freq_m else '?'
                ts_m = re.search(r'\[\s*(\d+)\s*ms\]', line)
                ts = int(ts_m.group(1)) if ts_m else 0
                clusters[cluster_id] = {
                    'lat': lat, 'lon': lon, 'freq': freq, 'ts': ts, 'hits': 0
                }

    # Count hits per cluster
    with open(logfile, 'r') as f:
        for line in f:
            m = re.search(r'CLUSTER #(\d+)', line)
            if m:
                cid = int(m.group(1))
                if cid in clusters:
                    clusters[cid]['hits'] += 1

    # Print results
    print('=' * 70)
    print('CLUSTER LOCATIONS')
    print('=' * 70)
    
    for cid, c in clusters.items():
        print(f"""
Cluster #{cid}:
  Location:  {c['lat']}, {c['lon']}
  Frequency: {c['freq']} MHz (K-band)
  Hits:      {c['hits']}
  Time:      {c['ts']/1000:.0f}s into drive ({c['ts']/60000:.1f} min)
  Maps:      https://maps.google.com/?q={c['lat']},{c['lon']}
""")

    # Calculate distances between clusters
    if len(clusters) >= 2:
        print('Distances between clusters:')
        ids = list(clusters.keys())
        for i in range(len(ids)):
            for j in range(i+1, len(ids)):
                c1, c2 = clusters[ids[i]], clusters[ids[j]]
                dist = haversine(c1['lat'], c1['lon'], c2['lat'], c2['lon'])
                print(f"  Cluster #{ids[i]} to #{ids[j]}: {dist:.0f} meters")
    
    return clusters


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: analyze_clusters.py <logfile>")
        sys.exit(1)
    analyze_clusters(sys.argv[1])
