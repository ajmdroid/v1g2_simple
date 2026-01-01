# V1 Gen2 Performance Testing Guide

This document describes the performance monitoring and testing infrastructure for the V1 Gen2 display firmware.

## Overview

The firmware includes a low-overhead observability system designed for embedded constraints:

- **Zero-cost counters**: Always-on metrics that don't impact hot paths
- **Sampled latency tracking**: 1-in-8 packet timing to measure BLE→display latency
- **Event ring buffer**: Fixed 64-entry debug log (no heap, no strings)
- **Compile-time gating**: `PERF_METRICS=0` for release builds

## Quick Start

### Enable Debug Metrics (Runtime)

```bash
# Enable periodic metrics printing (10s intervals)
curl -X POST http://v1display.local/api/debug/enable?enable=true

# View current metrics
curl http://v1display.local/api/debug/metrics

# View event ring buffer
curl http://v1display.local/api/debug/events
```

### Run Automated Tests

```bash
cd tools

# Basic 30-second test
./run_monitored_tests.sh

# Full test with upload
./run_monitored_tests.sh --upload --duration 60

# Parse existing log
python3 parse_metrics.py ../test_results/test_stream_*.log
```

## Metrics System

### Always-On Counters (`PerfCounters`)

These counters are always active with negligible overhead:

| Counter | Description |
|---------|-------------|
| `rxPackets` | Total BLE notifications received |
| `rxBytes` | Total bytes received |
| `queueDrops` | Packets dropped (queue full) |
| `queueHighWater` | Maximum queue depth seen |
| `parseSuccesses` | Successfully parsed packets |
| `parseFailures` | Parse failures (resync) |
| `reconnects` | BLE reconnection count |
| `disconnects` | BLE disconnection count |
| `displayUpdates` | Frames drawn |
| `displaySkips` | Updates skipped (throttled) |

### Sampled Latency (`PerfLatency`)

Only active when `PERF_METRICS=1`. Measures 1-in-8 packets to reduce overhead:

| Metric | Description |
|--------|-------------|
| `minUs` | Minimum BLE→flush latency (µs) |
| `maxUs` | Maximum BLE→flush latency (µs) |
| `avgUs` | Average latency |
| `sampleCount` | Number of samples |

### Event Ring Buffer

64-entry circular buffer for debugging without hot-path logging:

```cpp
// Log an event (constant-time)
EVENT_LOG(EVT_BLE_NOTIFY, length);
EVENT_LOG(EVT_ALERT_NEW, (band << 8) | strength);
EVENT_LOG(EVT_LATENCY_SPIKE, latency_us / 100);
```

Event types:
- `EVT_BLE_*`: Connection, notification, queue events
- `EVT_PARSE_*`: Packet parsing events
- `EVT_DISPLAY_*`: Display update events
- `EVT_ALERT_*`: Alert state changes
- `EVT_PUSH_*`: Auto-push events

## API Endpoints

### GET /api/debug/metrics

Returns current performance counters:

```json
{
  "rxPackets": 12345,
  "rxBytes": 98760,
  "parseSuccesses": 12340,
  "parseFailures": 5,
  "queueDrops": 0,
  "queueHighWater": 8,
  "displayUpdates": 6170,
  "displaySkips": 0,
  "latencyMinUs": 1200,
  "latencyAvgUs": 4500,
  "latencyMaxUs": 12000,
  "debugEnabled": true
}
```

### GET /api/debug/events

Returns event ring buffer:

```json
{
  "totalEvents": 150,
  "overflow": true,
  "events": [
    {"t": 1234567, "type": "BLE_NOTIFY", "data": 24},
    {"t": 1234570, "type": "PARSE_OK", "data": 5},
    {"t": 1234590, "type": "DISPLAY_UPDATE", "data": 45}
  ]
}
```

### POST /api/debug/enable?enable=true

Enable/disable periodic debug printing to serial.

### POST /api/debug/events/clear

Clear the event ring buffer.

## Test Scenarios

### 1. Stream Test (Default)

Basic connectivity and latency test:

```bash
./run_monitored_tests.sh --duration 60
```

**Pass criteria:**
- Latency max < 100ms
- Drop rate < 1%
- Queue high water < 32

### 2. Proxy Load Test

Test with JBV1 app connected as proxy client:

```bash
./run_monitored_tests.sh --scenario proxy --duration 120
```

1. Connect device to V1
2. Connect JBV1 app to proxy server
3. Start test
4. Drive past radar (or use K band tester)

### 3. Reconnect Stress Test

Test BLE reconnection stability:

```bash
./run_monitored_tests.sh --scenario reconnect --duration 300
```

1. Power cycle V1 multiple times during test
2. Walk device out of BLE range and back
3. Verify reconnection and metrics stability

## Interpreting Results

### Good Results

```
Latency (BLE→flush):   1200/4500/12000 µs
Drops:                 0 (0.00%)
Queue high water:      8
```

- Latency under 20ms average = excellent
- Zero drops = queue properly sized
- High water under 16 = headroom available

### Problem Signs

```
Latency (BLE→flush):   5000/45000/250000 µs
Drops:                 15 (0.12%)
Queue high water:      62
```

- High latency = display or SPI blocking
- Drops = queue too small or processing too slow
- High water near 64 = queue saturated

## Tuning Parameters

### platformio.ini

```ini
build_flags = 
    -DPERF_METRICS=1           ; Enable metrics (0 for release)
    -DPERF_SAMPLE_RATE=8       ; Sample 1/N packets
    -DPERF_REPORT_INTERVAL_MS=10000  ; Report every 10s
    -DPERF_LATENCY_ALERT_MS=100      ; Alert if > 100ms
    -DEVENT_RING_SIZE=64       ; Event buffer size
```

### Queue Size (main.cpp)

```cpp
bleDataQueue = xQueueCreate(64, sizeof(BLEDataPacket));
```

Increase if seeing drops, but uses more RAM.

### Display Throttle (main.cpp)

```cpp
const unsigned long DISPLAY_DRAW_MIN_MS = 20;  // ~50fps max
```

Increase to reduce CPU load, decrease for smoother updates.

## Debugging Tips

### Serial Commands

```bash
# Monitor serial output
pio device monitor

# Enable debug mode via serial (add to setup() for testing)
perfMetricsSetDebug(true);
```

### Dump Event Ring

When debugging an issue:

1. Trigger the issue
2. Immediately dump events: `curl http://v1display.local/api/debug/events`
3. Look for patterns (QUEUE_FULL before PARSE_FAIL, etc.)

### Common Issues

| Symptom | Likely Cause | Solution |
|---------|--------------|----------|
| High latency | Display SPI blocking | Check flush() timing |
| Queue drops | Processing too slow | Increase queue size |
| Latency spikes | WiFi/proxy interference | Prioritize BLE task |
| Event overflow | Too many events | Filter event logging |

## Release Build

For production, disable debug overhead:

```ini
; platformio.ini
build_flags = 
    -DPERF_METRICS=0
```

This removes:
- Sampled latency tracking
- Debug print functionality
- Latency alerts

Counters remain available via `/api/debug/metrics` for diagnostics.
