# V1 Simple — Lean Ship Plan

**Goal:** Stable, low-latency BLE display + proxy. Cut complexity, ship fast.

---

## 1. SCOPE-CUT MAP

### KEEP (Core)
| Module | Justification |
|--------|---------------|
| `ble_client.cpp` | **Non-negotiable.** V1 connection + proxy. Hot path. |
| `packet_parser.cpp` | **Non-negotiable.** Frame & parse V1 packets. Hot path. |
| `display.cpp` | **Non-negotiable.** Visual output. Hot path. |
| `touch_handler.cpp` | **Non-negotiable.** Tap-to-mute + profile cycle. |
| `settings.cpp` | **Non-negotiable.** NVS-based preferences. |
| `v1_profiles.cpp` | **Non-negotiable.** Auto-push profiles. |
| `battery_manager.cpp` | **Non-negotiable.** Power latch + percentage. |
| `wifi_manager.cpp` | **Keep** (web UI required). Minimize blocking. |

### CUT (Disabled via compile flag — already done)
| Module | Reason |
|--------|--------|
| `alert_logger.cpp` | SD card writes add latency/jitter in hot path. CSV logging not needed for core operation. |
| `alert_db.cpp` | SQLite queries block main loop. Not essential. |
| `serial_logger.cpp` | SD writes for every `SerialLog.print()` cause jitter. Use JBV1 logs instead. |

### LATER (Post-ship polish)
| Module | Reason |
|--------|--------|
| `time_manager.cpp` | NTP nice-to-have but not critical. Can init lazily if WiFi connected. |
| Replay mode (`#ifdef REPLAY_MODE`) | Dev/testing only. |
| Color preview | Nice UI demo, but cycles through bands—can cause confusion. |

---

## 2. HOT PATH ANALYSIS

**Hot path = BLE notify → queue → parse → display**

```
BLE Notify (ISR context)
    ↓
onV1Data() → xQueueSend(bleDataQueue, ...)   [~5 µs — non-blocking]
    ↓
loop() → processBLEData()
    ↓ xQueueReceive()
    ↓ forwardToProxy()                       [blocking BLE write — 1-5 ms]
    ↓ rxBuffer.insert()
    ↓ parser.parse()                         [~20 µs]
    ↓ display.update()                       [~15-30 ms SPI]
```

### Identified Issues

| Location | Issue | Impact |
|----------|-------|--------|
| `forwardToProxy()` | Called per packet inside queue loop | Blocking write per packet; batching could help |
| `rxBuffer` | `std::vector<uint8_t>` with `.insert()` | Potential heap churn on every packet |
| `SerialLog.*` | Printf inside hot path | SD I/O blocks even when "disabled" (still does Serial) |
| `alertDB.logAlert()` | Called after display update | Blocks if DB enabled |
| `display.update()` | Full redraw vs delta | Could skip if state unchanged |
| Display throttle | `DISPLAY_DRAW_MIN_MS = 20` | 50fps cap good, but "continue" skips parse too |

---

## 3. PRIORITIZED ACTION BACKLOG

### Chunk 1: Instrument latency (MEASURE FIRST)
**Goal:** Baseline BLE→screen latency before any changes.

**Changes:**
- Add `latencyMinUs`, `latencyMaxUs`, `latencyAvgUs` static counters in `processBLEData()`
- Calculate `uint32_t lat = micros() - (pkt.tsMs * 1000)` (approximate)
- Print summary every 5 seconds to Serial (not SerialLog)
- Add `queueOverflowCount` counter when `xQueueSend` fails

**Acceptance:**
- [ ] Serial output shows min/avg/max latency every 5s
- [ ] Queue overflow count visible
- [ ] No functional change

**Test:**
- Connect to V1, observe latency stats
- Generate alerts, check responsiveness

---

### Chunk 2: Remove Serial.print from hot path
**Goal:** Zero blocking I/O in BLE callback and parse loop.

**Changes:**
- Audit `processBLEData()` for any `Serial.*` or `SerialLog.*` calls inside the `while(true)` parse loop
- Move all prints outside the loop (summary at end) or behind `#ifdef DEBUG_HOT_PATH`
- Guard remaining prints with `if (DEBUG_HOT_PATH)` static constexpr bool

**Acceptance:**
- [ ] No Serial calls inside packet parse loop
- [ ] Build + run, same behavior
- [ ] Latency metrics should improve (measure before/after)

**Test:**
- Before/after latency comparison
- Visual responsiveness

---

### Chunk 3: Use static buffer instead of std::vector for rxBuffer
**Goal:** Eliminate heap allocations in hot path.

**Changes:**
- Replace `static std::vector<uint8_t> rxBuffer` with `static uint8_t rxBuffer[1024]; static size_t rxLen = 0;`
- Update all `.insert()`, `.erase()`, `.clear()` to use memcpy/memmove
- Keep existing framing logic

**Acceptance:**
- [ ] `heap_caps_get_free_size()` stable during sustained alerts
- [ ] No functional change in parsing

**Test:**
- Sustained alert stream (JBV1 or real V1)
- Heap free before/after should be identical

---

### Chunk 4: Consolidate proxy forwarding (single point)
**Goal:** Ensure no duplicate notifications to JBV1.

**Changes:**
- Verify `forwardToProxy()` is called exactly once per packet (currently in `processBLEData()` loop)
- Add counter: `proxyForwardCount` — print every 5s
- If duplicates found, trace and remove

**Acceptance:**
- [ ] JBV1 receives exactly N packets when V1 sends N packets
- [ ] Counter increments match V1 transmission rate

**Test:**
- JBV1 logging — check for duplicate alerts
- Counter should match packet parse count

---

### Chunk 5: Latest-wins display update (skip stale)
**Goal:** Only render the most recent state, discard backlog.

**Changes:**
- After draining queue, only render final parsed `DisplayState`
- Change: `while (xQueueReceive(...)) { ... }` accumulates all, then ONE `display.update()` at end
- Remove the `if (now - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) continue;` inside loop — throttle at end only

**Acceptance:**
- [ ] Under burst load, display shows final state (no stepping through stale)
- [ ] Latency metrics improve under load

**Test:**
- Trigger rapid alerts (JBV1 replay)
- Visual should snap to current, not animate through old

---

### Chunk 6: WiFi startup sequence (non-blocking or timeout)
**Goal:** WiFi init must not delay BLE beyond 10s max.

**Changes:**
- Current: `while (WiFi.status() != WL_CONNECTED && < 10s)` — already implemented
- Audit: ensure no blocking calls after this (NTP, web server bind)
- Move NTP sync to background (call in `loop()` once connected, not blocking setup)

**Acceptance:**
- [ ] Cold boot to BLE scan < 5s when WiFi unavailable
- [ ] WiFi connection does not block BLE reconnect

**Test:**
- Boot with no WiFi networks in range
- Time from power-on to "Scanning for V1..."

---

### Chunk 7: Simplify auto-push state machine
**Goal:** Reduce complexity, remove diff logic (causes confusion).

**Changes:**
- Remove `AutoPushLastApplied` diff tracking — always push full profile on tap
- Keep `startAutoPush()` but remove early-exit for "same profile"
- User taps = push. Period.

**Acceptance:**
- [ ] Every triple-tap pushes profile (no skipping)
- [ ] No "Skipping - V1 already matches" logs

**Test:**
- Triple-tap same profile twice — both should push
- Profile settings should apply

---

### Chunk 8: Mute stability filter tuning
**Goal:** Faster mute response without flicker.

**Changes:**
- Current: `MUTE_STABILITY_THRESHOLD = 5` packets
- Reduce to `3` packets (~60ms at 50Hz V1 rate)
- If still flickers, add hysteresis (unmute requires more packets than mute)

**Acceptance:**
- [ ] Tap-to-mute visual response < 100ms
- [ ] No color flicker during V1 logic mode auto-mute

**Test:**
- Tap mute — measure time to color change
- Drive past auto-muting signal — should stay stable

---

### Chunk 9: BLE reconnect hardening
**Goal:** Bulletproof reconnect after V1 power cycle or range loss.

**Changes:**
- Add exponential backoff on repeated connect failures (current: 1s fixed)
- After 5 failures, delete bond and restart scan
- Add `reconnectAttempts` counter, print to Serial

**Acceptance:**
- [ ] After V1 power cycle, reconnects within 30s
- [ ] No stuck state requiring device reboot

**Test:**
- Power cycle V1 mid-session
- Walk out of range and back

---

### Chunk 10: Web UI audit (cut non-essential endpoints)
**Goal:** Reduce web server surface area.

**Audit these endpoints:**
- `/api/logs/data` — **CUT** (requires alert_db)
- `/api/logs/clear` — **CUT**
- `/api/serial-log` — **CUT** (requires serial_logger)
- `/api/serial-log/clear` — **CUT**

**Keep:**
- `/api/status` — required
- `/api/settings` — required
- `/api/profiles/*` — required
- `/api/devices/*` — required
- `/api/command/*` — required (dark mode, mute)

**Acceptance:**
- [ ] Removed endpoints return 404
- [ ] Essential endpoints work

**Test:**
- Web UI manual test
- Profile save/load
- Settings change

---

## 4. MEASUREMENT PLAN

### Instrumentation (add to main.cpp)

```cpp
// Latency tracking (add near top of file)
static uint32_t latencyMinUs = UINT32_MAX;
static uint32_t latencyMaxUs = 0;
static uint32_t latencyTotalUs = 0;
static uint32_t latencyCount = 0;
static uint32_t queueOverflowCount = 0;
static uint32_t proxyForwardCount = 0;
static unsigned long lastMetricsPrint = 0;

// In processBLEData(), after xQueueReceive:
uint32_t latUs = (millis() - pkt.tsMs) * 1000; // rough
latencyMinUs = min(latencyMinUs, latUs);
latencyMaxUs = max(latencyMaxUs, latUs);
latencyTotalUs += latUs;
latencyCount++;

// In loop(), print metrics every 5s:
if (millis() - lastMetricsPrint > 5000) {
    lastMetricsPrint = millis();
    if (latencyCount > 0) {
        Serial.printf("[METRICS] Latency: min=%lu avg=%lu max=%lu us (n=%lu) overflow=%lu fwd=%lu heap=%lu\n",
            latencyMinUs, latencyTotalUs / latencyCount, latencyMaxUs, latencyCount,
            queueOverflowCount, proxyForwardCount, heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    }
    // Reset for next window
    latencyMinUs = UINT32_MAX;
    latencyMaxUs = 0;
    latencyTotalUs = 0;
    latencyCount = 0;
}
```

### Key Metrics

| Metric | Target | How to Measure |
|--------|--------|----------------|
| BLE→display latency | < 50ms avg, < 100ms max | `latencyAvgUs` / 1000 |
| Queue overflow rate | 0 under normal load | `queueOverflowCount` |
| Heap free | > 100KB stable | `heap_caps_get_free_size()` |
| Proxy forward count | = V1 packet count | `proxyForwardCount` vs JBV1 log |
| Reconnect time | < 10s after V1 power cycle | Stopwatch |

---

## 5. BUILD/TEST CHECKLIST

Run after **every** chunk:

### Connection Tests
- [ ] Cold boot → V1 connects
- [ ] V1 power cycle → reconnects within 30s
- [ ] Walk out of range → reconnects when back
- [ ] JBV1 proxy connects and receives alerts

### Display Tests
- [ ] Alert appears < 100ms after V1 detects
- [ ] Arrows show correct direction
- [ ] Signal bars animate smoothly
- [ ] Mute color change < 100ms after tap
- [ ] No flicker during V1 logic auto-mute
- [ ] Resting screen shows after alert clears

### Touch Tests
- [ ] Single tap mutes active alert
- [ ] Single tap does nothing when no alert
- [ ] Triple tap cycles profile (0→1→2→0)
- [ ] Triple tap blocked during active alert
- [ ] No missed touches, no double-triggers

### Profile Tests
- [ ] Triple tap pushes profile to V1
- [ ] Profile settings visible in JBV1
- [ ] Web UI profile save/load works
- [ ] Display mode (on/off) applies

### Web UI Tests
- [ ] Status endpoint returns V1 connection state
- [ ] Settings save persists across reboot
- [ ] Profile CRUD operations work
- [ ] Auto-push toggle works

### Battery Tests (if on battery)
- [ ] Percentage displays correctly
- [ ] Low battery warning at ~10%
- [ ] Critical shutdown at ~5%
- [ ] Power button hold → off

### Proxy Tests
- [ ] JBV1 sees all alerts V1 sees
- [ ] No duplicate alerts in JBV1
- [ ] Commands from JBV1 work (mute, mode)
- [ ] Proxy disconnect doesn't crash display

---

## 6. EXECUTION ORDER

```
Week 1: Measure + Stabilize Hot Path
├── Chunk 1: Instrument latency ✓
├── Chunk 2: Remove Serial from hot path
├── Chunk 3: Static buffer for rxBuffer
└── Chunk 4: Consolidate proxy forwarding

Week 2: Optimize + Simplify
├── Chunk 5: Latest-wins display update
├── Chunk 6: WiFi startup sequence
├── Chunk 7: Simplify auto-push
└── Chunk 8: Mute stability tuning

Week 3: Harden + Polish
├── Chunk 9: BLE reconnect hardening
└── Chunk 10: Web UI audit
```

---

## 7. RISK REGISTER

| Risk | Mitigation |
|------|------------|
| Static rxBuffer causes parse bugs | Thorough testing with chunked packets |
| Latest-wins loses important alerts | V1 sends at 50Hz, unlikely to miss |
| WiFi timeout delays BLE too much | Reduce timeout, add async option |
| Mute filter too aggressive | Keep adjustable, test in field |
| Web UI cuts break user workflows | Document removed features |

---

## 8. SUCCESS CRITERIA

**Ship when:**
- [ ] Latency < 50ms avg, < 100ms p99
- [ ] Zero queue overflows under sustained load
- [ ] Zero proxy duplicates
- [ ] Touch mute works 100% (no missed, no double)
- [ ] Profile push works 100%
- [ ] BLE reconnects within 30s after any disconnect
- [ ] Web UI core functions work
- [ ] Battery percentage accurate, shutdown works
- [ ] Heap stable (no leaks over 1hr test)

---

**Last updated:** 2025-01-01  
**Next action:** Implement Chunk 1 (latency instrumentation)
