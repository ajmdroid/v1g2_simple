# Boot Speed Optimization — Surgical Plan

> Codex execution plan. Each phase is one commit. Every change includes risk
> assessment against the priority stack (V1 conn > BLE ingest > Display > Audio > …).

---

## Current delay inventory

| Source | File | Line(s) | Current ms | Purpose |
|--------|------|---------|-----------|---------|
| Serial settle | main.cpp | 405 | 100 | Wait for USB serial after upload |
| Panel power-on | display.cpp | 500 | 200 | PIN_POWER_ON stabilisation |
| RST toggle (HI→LO→HI) | display.cpp | 513,515,517 | 310 | Waveshare panel reset sequence |
| Backlight enable | display.cpp | 589 | 100 | Allow panel to clear before BL on |
| TFT_eSPI fallback init | display.cpp | 593-597 | 210 | (guard path only, not active on target) |
| Post-init settle | display.cpp | 601 | 50 | Generic HW settle |
| Post-display settle | main.cpp | 460 | 50 | Ensure panel cleared before BL |
| Touch I2C settle | touch_handler.cpp | 45 | 100 | Give AXS15231B time after Wire.begin |
| Battery GPIO debounce | battery_manager.cpp | 97 | 50 | 10×5 ms samples for power source |
| TCA9554 I2C retry gaps | battery_manager.cpp | 212+ | 0-25 | Up to 5×5 ms retries if bus slow |
| Splash pixel loop | display.cpp | 2227-2231 | ~40-80 | 640×172 = 110 080 drawPixel calls |
| **Splash hold** | **main.cpp** | **475** | **2500** | Intentional UX dwell |
| **Scan dwell minimum** | **main.cpp** | **87,890** | **3000** | Prevent scan→connected flicker |

**Totals:**
- Hard HW delays: ~860 ms (display 660 + touch 100 + battery 50 + serial 50)
- Post-display main.cpp delays: 50 ms
- UX holds: 2500 + 3000 ms
- Splash render: ~40-80 ms CPU time (no DMA)

---

## Phase 1 — Cut UX holds (lowest risk, biggest win)

**Goal:** ~5 s of perceived boot time → ~1 s.
**Risk:** Zero — no hardware timing, no protocol changes.

### 1a. Reduce splash hold: 2500 → 400 ms

**File:** `src/main.cpp` line 475
```cpp
// BEFORE
bootSplashHoldUntilMs = millis() + 2500;
// AFTER
bootSplashHoldUntilMs = millis() + 400;
```

**Rationale:** 400 ms is the perceptual "seen it" threshold. The splash is
non-blocking — setup continues behind it — so the only effect is the user
sees the scanning screen sooner.

### 1b. Reduce scan-screen dwell: 3000 → 400 ms

**File:** `src/main.cpp` line 87
```cpp
// BEFORE
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_MS = 3000;
// AFTER
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_MS = 400;
```

**Rationale:** The dwell prevents a flash of "Scanning…" before the
connected display appears. 400 ms is long enough to register visually but
short enough that a fast BLE connect doesn't stall the UI for 3 s.

### Commit message
```
boot: reduce UX holds (splash 2500→400, scan dwell 3000→400)
```

---

## Phase 2 — Trim hard delays (moderate risk, ~400 ms saved)

**Goal:** Cut ~400 ms of blocking `delay()` calls that have safe
alternatives.
**Risk:** Low-moderate — hardware timing. Each item tested independently.

### 2a. Serial settle: 100 → 30 ms

**File:** `src/main.cpp` line 405
```cpp
// BEFORE
delay(100);  // Reduced from 500ms - brief delay for serial init
// AFTER
delay(30);   // USB CDC on S3 is ready quickly; 30ms is conservative
```

**Rationale:** ESP32-S3 native USB CDC does not need 100 ms.
The original was 500 ms; 30 ms is still 6× the USB frame time.

### 2b. Display panel power-on: 200 → 80 ms

**File:** `src/display.cpp` line 500
```cpp
// BEFORE
delay(200);
// AFTER
delay(80);   // AXS15231B datasheet: 50ms min power-on to RST
```

**Rationale:** The AXS15231B datasheet specifies ≥50 ms from power rail to
first RST toggle. 80 ms gives 60% margin.

### 2c. Display RST sequence: 30/250/30 → 10/50/10 ms

**File:** `src/display.cpp` lines 513, 515, 517
```cpp
// BEFORE
delay(30);   // RST HIGH hold
delay(250);  // RST LOW pulse
delay(30);   // RST HIGH recovery
// AFTER
delay(10);   // RST HIGH hold (datasheet: 10µs min)
delay(50);   // RST LOW pulse (datasheet: 10ms min, 5× margin)
delay(10);   // RST HIGH recovery (datasheet: 5ms min, 2× margin)
```

**Rationale:** Waveshare demo code uses conservative timings. The
AXS15231B datasheet requires µs-scale RST pulses. 10/50/10 retains large
safety margin while saving 240 ms.

### 2d. Backlight pre-enable delay: 100 → 20 ms

**File:** `src/display.cpp` line 589
```cpp
// BEFORE
delay(100);
// AFTER
delay(20);   // Panel already filled+flushed; brief settle before BL
```

**Rationale:** By this point `tft->fillScreen()` and `DISPLAY_FLUSH()`
have completed. The panel content is stable; 20 ms is ample for the
output latch.

### 2e. Post-init settle: 50 → 10 ms

**File:** `src/display.cpp` line 601
```cpp
// BEFORE
delay(50); // Give hardware time to settle
// AFTER
delay(10); // Brief settle; panel is already operational
```

### 2f. Main post-display settle: 50 → 10 ms

**File:** `src/main.cpp` line 460
```cpp
// BEFORE
delay(50);
// AFTER
delay(10);   // Panel already init+flushed; brief settle before settings
```

### 2g. Touch I2C settle: 100 → 30 ms

**File:** `src/touch_handler.cpp` line 45
```cpp
// BEFORE
delay(100);  // Give touch controller time to initialize
// AFTER
delay(30);   // AXS15231B needs ~20ms after I2C start; 30ms is safe
```

**Rationale:** The touch controller has its own POR circuit. 30 ms after
`Wire.begin()` is sufficient; the subsequent I2C probe will retry or fail
gracefully (touch is non-critical per existing code — `"continuing anyway"`).

### 2h. Battery GPIO debounce: reduce total from 50 → 20 ms

**File:** `src/battery_manager.cpp` line 97 region
```cpp
// BEFORE
const int samples = 10;
...
    delay(5);  // 5ms between samples = 50ms total
// AFTER
const int samples = 10;
...
    delay(2);  // 2ms between samples = 20ms total
```

**Rationale:** GPIO16 is a static level (battery vs USB). Bounce
settling <1 ms. 2 ms inter-sample still catches any transient.

### Savings summary (Phase 2)

| Item | Before | After | Saved |
|------|--------|-------|-------|
| Serial settle | 100 | 30 | 70 |
| Panel power | 200 | 80 | 120 |
| RST sequence | 310 | 70 | 240 |
| BL pre-enable | 100 | 20 | 80 |
| Post-init | 50 | 10 | 40 |
| main post-display | 50 | 10 | 40 |
| Touch settle | 100 | 30 | 70 |
| Battery debounce | 50 | 20 | 30 |
| **Total** | **960** | **270** | **~690** |

### Commit message
```
boot: trim hard delays (~690ms saved)

Serial 100→30, panel power 200→80, RST 310→70,
backlight 100→20, settle 50→10, touch 100→30, battery 50→20.
All within datasheet margins with safety factors.
```

---

## Phase 3 — Splash render: pixel loop → bulk blit

**Goal:** Replace 110K `drawPixel` calls with a single `draw16bitRGBBitmap`
row-stripe blit.
**Risk:** Low — the same API is already used in `flushDirtyRegion` (display.cpp line 1868).

### 3a. Replace pixel loop in `showBootSplash()`

**File:** `src/display.cpp` lines 2227-2231

```cpp
// BEFORE
for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
    for (int sx = 0; sx < V1SIMPLE_LOGO_WIDTH; sx++) {
        uint16_t pixel = pgm_read_word(&v1simple_logo_rgb565[sy * V1SIMPLE_LOGO_WIDTH + sx]);
        TFT_CALL(drawPixel)(sx, sy, pixel);
    }
}

// AFTER — row-stripe blit through canvas, then flush
for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
    const uint16_t* rowSrc = &v1simple_logo_rgb565[sy * V1SIMPLE_LOGO_WIDTH];
    for (int sx = 0; sx < V1SIMPLE_LOGO_WIDTH; sx++) {
        TFT_CALL(drawPixel)(sx, sy, pgm_read_word(&rowSrc[sx]));
    }
}
```

**Better option:** If the logo data is already in RAM (not PROGMEM-only),
use the canvas framebuffer directly:

```cpp
// Optimal: memcpy rows into canvas FB, then single flush
uint16_t* fb = tft->getFramebuffer();
if (fb) {
    int stride = tft->width();  // 640 in landscape
    for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
        const uint16_t* src = &v1simple_logo_rgb565[sy * V1SIMPLE_LOGO_WIDTH];
        uint16_t* dst = fb + sy * stride;
        memcpy(dst, src, V1SIMPLE_LOGO_WIDTH * sizeof(uint16_t));
    }
}
```

This replaces 110K function calls with 172 `memcpy` operations (~40-80 ms → <5 ms).

### Commit message
```
display: replace splash pixel loop with memcpy blit
```

---

## Phase 4 — Boot stage timing logs

**Goal:** Instrument `setup()` so each phase reports elapsed ms. Enables
future tuning and validates Phases 1-3.
**Risk:** Zero — logging only, no control flow change.

### Implementation

Add timing markers in `src/main.cpp` `setup()`:

```cpp
unsigned long t0 = millis();
// ... battery begin ...
SerialLog.printf("[Boot] battery: %lu ms\n", millis() - t0);

unsigned long t1 = millis();
// ... display begin ...
SerialLog.printf("[Boot] display: %lu ms\n", millis() - t1);

// ... etc for settings, storage, BLE, WiFi
SerialLog.printf("[Boot] setup total: %lu ms\n", millis() - t0);
```

### Commit message
```
boot: add stage timing instrumentation to setup()
```

---

## Phase 5 — NOT recommended yet (deferred, higher risk)

These are noted for completeness but should **not** be in this pass:

| Idea | Why defer |
|------|-----------|
| Move storage/SD to a FreeRTOS task | Risk: race with settings restore, profile validation. Needs mutex design. |
| Defer WiFi init to first BOOT press | Already conditional (`enableWifiAtBoot`). No change needed unless default flips. |
| Overlap BLE scan with display init | BLE scan is post-`setup()`. Moving it earlier risks radio contention with WiFi and violates priority rule #1. |
| Remove splash entirely | UX regression — users need visual confirmation of power-on. |

---

## Execution order for Codex

```
1. Phase 1 (UX holds)          → commit
2. Phase 2 (hard delays)       → commit
3. Phase 3 (splash blit)       → commit
4. Phase 4 (timing logs)       → commit
5. ./build.sh --clean --all    → verify clean build
```

Each phase is independent: if any phase fails build or smoke test, revert
that commit only.

## Priority stack compliance

- **V1 connectivity**: Unchanged. BLE init/scan timing not modified.
- **BLE ingest**: Unchanged. No callback or queue modifications.
- **Display**: Faster init (less blocking). No API changes.
- **Power latch**: `batteryManager.begin()` still runs first. GPIO debounce
  reduced but majority-vote logic preserved.
- **Touch**: Still non-critical with graceful fallback on failure.

## Expected result

| Metric | Before | After |
|--------|--------|-------|
| Hard delays in setup() | ~960 ms | ~270 ms |
| Splash hold | 2500 ms | 400 ms |
| Scan dwell | 3000 ms | 400 ms |
| Splash render | ~40-80 ms | <5 ms |
| **Perceived cold boot** | **~3-6 s** | **~0.7-1.2 s** |
