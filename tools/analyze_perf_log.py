#!/usr/bin/env python3
"""Analyze perf metrics from debug.log"""
import re, sys

logfile = sys.argv[1] if len(sys.argv) > 1 else '/Volumes/SDCARD/debug.log'
lines = open(logfile).readlines()

records = []
for i, line in enumerate(lines):
    m = re.search(r'"millis":(\d+)', line)
    msg = re.search(r'"message":"(.+?)"', line)
    if m and msg:
        millis = int(m.group(1))
        metrics = {}
        for kv in re.findall(r'(\w+)=(\d+)', msg.group(1)):
            metrics[kv[0]] = int(kv[1])
        metrics['millis'] = millis
        metrics['line'] = i + 1
        records.append(metrics)

print(f'Total records: {len(records)}')

sessions = []
start = 0
for i in range(1, len(records)):
    if records[i]['millis'] < records[i-1]['millis']:
        sessions.append(records[start:i])
        start = i
sessions.append(records[start:])

print(f'Sessions found: {len(sessions)}')
for si, sess in enumerate(sessions):
    dur = sess[-1]['millis'] / 1000
    print(f'  Session {si+1}: {len(sess)} records, {dur:.0f}s ({dur/60:.1f}min)')

print()

for si, sess in enumerate(sessions):
    print(f'=== SESSION {si+1} ===')
    for k in ['loopMax_us', 'bleConnMax_us', 'bleDrainMax_us', 'dispMax_us', 'sdMax_us', 'wifiMax_us', 'lockoutMax_us']:
        vals = [r.get(k, 0) for r in sess]
        if not vals: continue
        peak = max(vals)
        peak_idx = vals.index(peak)
        avg = sum(vals) / len(vals)
        p95 = sorted(vals)[int(len(vals) * 0.95)]
        print(f'  {k}: peak={peak} p95={p95} avg={avg:.0f} (peak line {sess[peak_idx]["line"]})')

    heaps = [r.get('heapMin', 0) for r in sess]
    dmas = [r.get('dmaMin', 0) for r in sess]
    dblks = [r.get('dmaBlock', 0) for r in sess]
    print(f'  heapMin: start={heaps[0]} min={min(heaps)} end={heaps[-1]} delta={heaps[-1]-heaps[0]}')
    print(f'  dmaMin:  start={dmas[0]} min={min(dmas)} end={dmas[-1]}')
    print(f'  dmaBlock: min={min(dblks)}')

    drops = sum(1 for r in sess if r.get('qDrop', 0) > 0)
    print(f'  qDrop > 0: {drops} records')

    big = [r for r in sess if r.get('loopMax_us', 0) > 100000]
    if big:
        print(f'  Loops > 100ms: {len(big)}')
        for r in big:
            print(f'    line {r["line"]}: loop={r["loopMax_us"]}us bleCon={r.get("bleConnMax_us",0)}us disp={r.get("dispMax_us",0)}us wifi={r.get("wifiMax_us",0)}us')

    disp_vals = [r.get('dispMax_us', 0) for r in sess]
    print(f'  Display: {sum(1 for d in disp_vals if d > 0)} alert, {sum(1 for d in disp_vals if d == 0)} idle')
    print()
