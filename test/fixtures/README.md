# Test Fixtures

This directory contains captured BLE packet traces for replay testing.

## Creating Fixtures

### 1. Enable BLE Debug Logging

In the web UI, go to **Dev** → **Log Categories** and enable:
- ✅ BLE (captures raw packet hex)
- ✅ Alerts (captures alert state changes)

### 2. Drive / Trigger Alerts

Go for a drive or use another method to generate alerts. The debug log will capture:
- Raw BLE packets with timestamps: `[BLE:PKT] ts=12345 len=19 hex=AA040A43...`
- Alert state changes: `[Alerts] count=1 pri=Ka dir=FRONT freq=34712...`

### 3. Download the Log

Go to **Dev** → **Download Log** to get `debug.log`

### 4. Parse and Extract Fixtures

```bash
# See statistics and find interesting segments
python tools/parse_ble_log.py debug.log --stats --segments

# Extract a specific segment (e.g., segment 0)
python tools/parse_ble_log.py debug.log --extract-segment 0 -o test/fixtures/ka_alert.json

# Extract full log
python tools/parse_ble_log.py debug.log -o test/fixtures/full_drive.json
```

## Fixture Format

```json
{
  "metadata": {
    "source": "debug.log",
    "captured": "2026-01-30T12:00:00",
    "packet_count": 1234,
    "stats": {
      "duration_ms": 300000,
      "packet_types": {"DisplayData": 500, "AlertData": 100}
    }
  },
  "packets": [
    {"ts": 12345, "len": 19, "hex": "AA040A430C0401050000D02F0100000001E8AB"},
    ...
  ]
}
```

## Using Fixtures in Tests

See `test/test_perf/test_replay.cpp` for example usage:

```cpp
// Load fixture and feed packets to parser
ReplayFixture fixture("test/fixtures/ka_alert.json");
for (const auto& pkt : fixture.packets) {
    feedPacket(pkt.hex, pkt.ts);
    // Assert latency, state, etc.
}
```

## Included Fixtures

| File | Description | Packets | Duration |
|------|-------------|---------|----------|
| `sample_ka.json` | Single Ka alert cycle | ~50 | 5s |
| *(add your captures here)* | | | |
