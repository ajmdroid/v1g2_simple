# V1 Simple — Production Readiness Review

**Reviewer**: Senior Engineer (external review)
**Date**: April 12, 2026
**Firmware version**: v4.0.1
**Hardware**: Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B 640×172 QSPI)

---

## Executive Summary

This is a remarkably well-engineered first project. The architecture discipline, test infrastructure, and documentation quality exceed what I typically see from teams of experienced embedded developers, let alone on a solo first project. The testing pyramid — broad native coverage, tracked mutation lanes, targeted bug-pattern detectors, semantic guards, contract verification, replay fixtures, hardware device lanes, and a three-tier CI pipeline (PR → nightly → pre-release with hardware) — is genuinely strong for an ESP32 project.

The project is production-ready with caveats. After secondary verification, I see two issues that should be resolved before shipping, several medium-priority items to address in the near term, and one previously classified P0 that is now retracted for the current driver stack.

> **Verification note**: Every issue finding in this review has been independently verified against the source code by tracing full call chains, examining surrounding context, and checking for compensating mechanisms. Corrections from verification are noted inline where the initial assessment was inaccurate. Inventory-style headline counts are only retained where this pass could support them directly.

---

## P0 — Fix Before Shipping

### 1. Loop Duration SLO Metric Does Not Measure What It Claims

**Location**: `src/main.cpp:1140`, `src/modules/system/loop_telemetry_module.cpp`

The `loopMax_us` metric (SLO limit: ≤250ms) is recorded by `loopTelemetryModule.process(loopStartUs)` at line 1140 of main.cpp — which runs *after* WiFi but *before* `processLoopFinalizePhase()`. The finalize phase includes `LoopPostDisplayModule` (auto-push + connection dispatch), `PeriodicMaintenanceModule`, and `LoopTailModule` (BLE drain + yield). The actual end-to-end loop duration is computed in `LoopTailModule::process()` at `main_loop_phases.cpp:155`, but that value is only stored in `mainRuntimeState.lastLoopUs` — it is not fed to the perf SLO recorder.

This means your SLO gate is measuring an incomplete loop. If the finalize phase ever takes meaningful time (and BLE drain under backpressure certainly could), you'd pass the SLO check while actually exceeding 250ms.

> **Verified**: Traced the full chain. `loopTelemetryModule.process()` calls into `perfRecordLoopJitterUs()` (wired at main.cpp:606-607), which updates `perfExtended.loopMaxUs` via max-tracking (perf_metrics.cpp:803-807). The `loopTailModule.process()` return value at main_loop_phases.cpp:155 is stored in `mainRuntimeState.lastLoopUs` at main.cpp:1152 but is never passed to any perf recording function. Confirmed: the SLO metric excludes finalize-phase time.

**Recommendation**: Move `perfRecordLoopJitterUs()` into `LoopTailModule`, or record it from the `lastLoopUs` value returned by `loopTailModule.process()`. This is a one-line change.

### 2. Settings Mode Early Return Skips BLE Drain and Finalize-Phase Work

**Location**: `src/main.cpp:1069-1071`

```cpp
if (shouldReturnEarlyFromLoopPowerTouchPhase(now, loopStartUs)) {
    return;  // Skip normal loop processing while in settings mode.
}
```

When the user is interacting with the touch settings UI, the loop returns before BLE ingest/drain, periodic maintenance, and finalize-phase loop-duration recording. This does not fully skip telemetry: `LoopPowerTouchModule::process()` still records touch timing, loop jitter, and heap stats before returning early. The real production risk is that packet drain stops while notifications continue to enqueue.

More critically, the original "minimum fix" recommendation was incomplete. `LoopTailModule::process()` only drains when `bleBackpressure` is true, but `BleQueueModule::backpressureActive_` is refreshed inside `BleQueueModule::process()`, not in `onNotify()`. While the UI is in settings mode, queue pressure can grow without that flag being refreshed, so "run tail before return" can still skip drain.

> **Verified**: Confirmed no compensating mechanism exists. BLE data arrives via `onNotify()` in the BLE task context and enqueues to the FreeRTOS queue regardless of settings mode. Both normal drain points (`LoopIngestModule` and `LoopTailModule`) are in code paths bypassed by the early return. `LoopPowerTouchModule::process()` does record loop jitter, heap stats, and touch timing before returning early, so this is not a total telemetry blind spot. `BleQueueModule::isBackpressured()` returns a cached flag that is only updated by `refreshBackpressureState()` inside `BleQueueModule::process()`. Extended settings interaction under sustained traffic can therefore overflow the queue without ever tripping the stale backpressure gate.

**Recommendation**: Keep a reduced always-run path for settings loops that unconditionally drains `bleQueueModule.process()`, preserves end-of-loop duration recording, and performs the required yield/maintenance work without re-entering display/WiFi processing. The power/touch early return should gate the *middle* of the loop, not the ingest/tail path. Do not rely on `loopTailModule.process()` alone as the minimal fix.

### 3. ~~No DMA Completion Verification on Display Flush~~ — RETRACTED

> **Verification correction**: This is not a current bug on the pinned AXS15231B QSPI driver stack. `Arduino_Canvas::flush()` delegates to `Arduino_TFT::draw16bitRGBBitmap(uint16_t*)`, and the `Arduino_ESP32QSPI` transport completes each write with `spi_device_polling_end(..., portMAX_DELAY)` via `POLL_END()` before returning. There is no evidence of a live framebuffer race on the current stack. The only remaining action here is documentation hygiene: if the project upgrades Arduino_GFX or changes display buses, re-verify this assumption.

---

## P1 — Should Fix Soon After Ship

### 4. ~~No Hardware Watchdog Timer (TWDT)~~ — RETRACTED

> **Verification correction**: The original claim was **wrong**. The TWDT IS enabled in `sdkconfig.defaults` (lines 28-32): `CONFIG_ESP_TASK_WDT_EN=y` with `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`. The Arduino framework's idle task hook automatically feeds the TWDT when the scheduler runs, and the unconditional `vTaskDelay(1)` in `LoopTailModule` ensures the idle task runs every iteration. The project has both hardware TWDT (5s) and a logical connection-state watchdog (1s). No action needed — this finding was inaccurate.

### 5. Battery Shutdown Path Blocks on I2C

**Location**: `src/battery_manager.cpp` (shutdown sequence, lines ~467-547)

The power-off sequence writes to the TCA9554 I2C expander to drop the power latch. The latch drop call uses `setTCA9554PinWithBudget()` with a 250ms mutex timeout and up to 5 retries (not 50ms as originally stated — corrected after verification).

> **Verification correction**: The critical latch-drop timeout is **250ms** (`pdMS_TO_TICKS(250)` at line 504), not 50ms. The 50ms timeout exists in the general-purpose `setTCA9554Pin()` helper (line 238) but is not used during shutdown. More importantly, the **verification read** at lines 510-526 — which checks whether the latch actually dropped — has **zero timeout protection**. It calls `Wire.beginTransmission()`, `Wire.endTransmission()`, and `Wire.requestFrom()` with no explicit timeout. If the I2C bus is stuck at that point, the device hangs indefinitely before reaching the deep sleep fallback. The full shutdown timeline is: ~2.5s backlight fade → 250ms mutex + retries → unprotected verification read → 500ms rail collapse wait → deep sleep fallback.

**Recommendation**: Prefer a bounded verification read over adding more asynchronous shutdown control flow. Either configure an I2C timeout for the readback path or skip readback entirely during critical battery shutdown and fall through to the existing 500ms rail-collapse wait / deep-sleep fallback.

### 6. Legacy Dirty Region Code Is Dead but Still Executing

**Location**: `src/display_frequency.cpp`, `src/display_cards.cpp`

The `frequencyDirtyX_/Y_/W_/H_` fields are still being computed and written by the frequency and card rendering code, but are never read — the pipeline switched to full-canvas flush. This is dead code that runs every render frame in the hot path.

> **Verified**: The fields are declared in `display.h` (lines 210-220) with an existing comment: *"Legacy dirty region tracking — still written by display_frequency.cpp and display_cards.cpp but no longer read after the display pipeline rewrite."* `markFrequencyDirtyRegion()` is called at 5 locations in display_frequency.cpp and `secondaryCardsRenderDirty_` is set in display_cards.cpp:98. Exhaustive search confirms none of these fields are ever read. The codebase already documents them as legacy — they just haven't been removed yet. Note: `flushRegion()` itself IS called from other paths (display_screens.cpp, touch_ui_module.cpp), but those callers pass their own coordinates — they don't read the dirty region fields.

**Recommendation**: Remove the dirty region writes and the fields. Clean dead code in the render hot path. Low risk, high clarity.

### 7. Unconditional `vTaskDelay(1)` Every Loop

**Location**: `src/modules/system/loop_tail_module.cpp:25-27`

```cpp
if (providers.yieldOneTick) {
    providers.yieldOneTick(providers.yieldContext);
}
```

This yield runs unconditionally — not gated on backpressure, overload, or any other condition. On ESP32 with the default 1ms tick, this means the main loop can never exceed ~1000 Hz. That's probably fine for this application, but it's worth being intentional about it.

> **Verified**: The yield is bound to `vTaskDelay(pdMS_TO_TICKS(1))` via a lambda at main.cpp:595-597. In `loop_tail_module.cpp`, the `bleBackpressure` parameter gates only the BLE drain block (lines 8-23); the yield at lines 25-27 is guarded only by a null-pointer check on the function pointer, not by any runtime condition. Confirmed unconditional. Note: this unconditional yield is also what keeps the hardware TWDT fed (see retracted P1-4), so removing it would require an explicit `esp_task_wdt_reset()` call.

**Recommendation**: Gate the yield on `bleBackpressure || overloadThisLoop`, or document the intentional 1ms floor as a design decision. If it's intentional (to prevent FreeRTOS starvation and feed the TWDT), add a comment. If you gate it, add an explicit TWDT feed.

---

## P2 — Advisory / Minor

### 8. WiFi Password Storage Uses XOR Obfuscation

`settings_nvs.cpp` stores WiFi client passwords in NVS with XOR obfuscation. This is not encryption — it's trivially reversible by anyone who can read the flash. For a radar detector display this is acceptable (the threat model doesn't include flash extraction attacks), but if you ever document security properties, don't call it encryption.

### 9. Mixed Timing Types

Timing variables alternate between `unsigned long` (Arduino `millis()` return type) and `uint32_t`. Both are 32 bits on ESP32, but the inconsistency makes it harder to grep for timing-related code. Standardizing on `uint32_t` everywhere (with a comment noting it wraps at ~49.7 days) would improve readability. Not worth a dedicated cleanup pass, but worth enforcing in new code.

### 10. Row-by-Row SPI Flush Workaround

`display.cpp:flushRegion()` uses row-by-row SPI writes to work around an AXS15231B address-window bug. This means a partial flush of a 50-row region results in 50 separate SPI transactions instead of 1.

> **Verification correction**: The original claim that this path is unused was **wrong**. `flushRegion()` is actively called from `display_screens.cpp:130,134` (profile flush), `main.cpp:390` (left column flush), and `touch_ui_module.cpp:75,119` (OBD badge flush). These are non-hot paths (profile changes, UI badge updates), but the row-by-row workaround is exercised in production. The comment at lines 385-388 already documents the panel bug well.

### 11. `audio_process_amp_timeout()` Called Every Loop

**Location**: `src/main.cpp:1049`

This runs every single loop iteration to check if the amplifier should be powered down after 3 seconds of inactivity. The check itself is cheap (a millis comparison), but it's an unnecessary call on 99.9% of iterations. A modulo counter (every 100th loop) would be functionally identical and marginally cleaner. Very low priority.

### 12. OTA Partition Layout

The partition table has dual OTA slots (app0 + app1) at 6.75 MB each, which is excellent for safe OTA updates. The `otadata` partition is present. I don't see OTA update code in the firmware — if OTA is planned for production, this infrastructure is ready but the implementation would need its own review. If OTA is not planned, consider reclaiming the second app slot for a larger LittleFS partition (currently 2.3 MB for audio + web assets).

---

## What's Genuinely Excellent

I want to be specific about what's working well, because these patterns should be preserved and extended:

**Architecture discipline**: The `begin()` injection pattern, the ban on `std::function`, the `Providers` struct for testable modules — this is textbook embedded architecture. The fact that there are zero `TODO`/`FIXME` comments in the source tree is remarkable. (Verified: `std::function` appears only once in src/, as a design-reminder comment in `alp_runtime_module.h`. Zero actual uses in module wiring.)

**Test infrastructure**: The native test surface is broad, the mutation catalog is integrated into CI workflows, and `check_bug_patterns.py` currently implements 5 targeted bug-pattern detectors. The bug pattern scanner trades quantity for quality — the patterns it does check (millis wraparound, int16 accumulator overflow, geo distance without cos(latitude), BLE callback blocking, raw heap allocation) are context-aware and domain-specific. The three-tier CI pipeline (PR → nightly → pre-release with hardware) is how professional firmware shops operate.

**Memory management**: PSRAM-aware allocation with internal SRAM fallback (verified: all `heap_caps_malloc` calls follow the try-PSRAM-then-INTERNAL pattern), proper static allocation for audio task stacks (with the comment explaining *why* it can't be in PSRAM), bounded BLE queues with backpressure — all correct. Atomic memory ordering in BLE callbacks verified correct (acquire on load, release on store).

**Observability**: Per-subphase render timing, loop jitter tracking, heap sampling, DMA floor monitoring, CSV perf logging with schema versioning. You can diagnose production issues from this data. Most embedded projects ship with `Serial.println()` and hope.

**The CLAUDE.md**: The Valentine Philosophy section isn't just good documentation — it's a design constitution. Every feature decision can be traced back to a principle. This is how you prevent scope creep on a solo project.

---

## Recommendation

Ship it. Address the remaining P0 items first (they're small changes), schedule P1 items for the first post-ship maintenance cycle. P2 items can be addressed opportunistically.

The testing infrastructure gives high confidence that the P0 fixes won't introduce regressions. Run `scripts/ci-test.sh` after each fix, and you're good.

---

## Appendix: Verification Results

Issue claims in this review were independently verified by tracing full call chains in the source code. Inventory-style headline counts are marked separately where this pass did not regenerate them. Here's the scorecard:

| Finding | Original Claim | Verification Result |
|---|---|---|
| P0-1: Loop SLO metric | Incomplete measurement | **Confirmed** — metric recorded mid-loop, misses finalize phase |
| P0-2: Settings early return | Skips BLE drain | **Confirmed in substance** — skips BLE drain and finalize work; partial telemetry still records during settings mode, and the prior tail-only fix was insufficient because backpressure state is refreshed in `BleQueueModule::process()` |
| P0-3: DMA completion | No explicit sync | **Retracted** — current `Arduino_ESP32QSPI` path blocks on `spi_device_polling_end()` before returning |
| P1-4: No hardware TWDT | Not configured | **Retracted** — TWDT IS enabled in sdkconfig.defaults (5s timeout) |
| P1-5: Battery I2C blocking | 50ms timeout risk | **Corrected** — timeout is 250ms, not 50ms; verification read at lines 510-526 is worse (zero timeout) |
| P1-6: Dead dirty regions | Written but never read | **Confirmed** — already documented as legacy in display.h comment |
| P1-7: Unconditional yield | vTaskDelay(1) every loop | **Confirmed** — bound to lambda at main.cpp:595-597, not gated on backpressure |
| P2-8: WiFi XOR obfuscation | Not real encryption | **Confirmed** — `xorObfuscate()` in settings_nvs.cpp:37-49 |
| P2-9: Mixed timing types | unsigned long vs uint32_t | **Confirmed** — inconsistent across display, audio, battery, perf code |
| P2-10: Row-by-row flush unused | Dead code path | **Corrected** — `flushRegion()` IS called from 3 active paths |
| P2-11: Amp timeout every loop | Unnecessary frequency | **Confirmed** — cheap check, negligible impact |
| P2-12: OTA slots unused | No OTA implementation | **Confirmed** — partition layout ready, zero OTA code |
| Positive: Zero TODO/FIXME | Clean codebase | **Confirmed** |
| Positive: std::function ban | Enforced | **Confirmed** — only a comment reference exists |
| Positive: PSRAM fallback | Consistent pattern | **Confirmed** — all heap_caps_malloc calls follow dual-tier pattern |
| Positive: Atomic ordering | Correct in BLE callbacks | **Confirmed** — acquire/release pairs verified |
| Positive: 96+ test suites | Large test suite | **Not independently revalidated** — this pass confirms broad native coverage, but not a regenerated numeric suite count |
| Positive: 60+ bug patterns | Domain-specific analyzer | **Corrected** — `check_bug_patterns.py` currently defines 5 targeted patterns |
| Positive: 20+ mutations | Mutation catalog | **Not independently revalidated** — tracked mutation lanes exist in CI/docs, but this pass did not regenerate a numeric count |
