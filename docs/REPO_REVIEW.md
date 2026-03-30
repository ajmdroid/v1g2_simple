# V1G2 Simple — Honest Repo Review

**Date:** March 30, 2026
**Scope:** Full codebase review against `v1_simple.instructions.md` and `ARCHITECTURE.md`
**Codebase:** ~29K LOC in `src/`, ~27K LOC in `src/modules/`, ~1,400 unit tests, 5 CI workflows

---

## Overall Verdict

This is a seriously impressive first project. The architecture is mature, the priority model is respected throughout, the dependency injection is consistent, and the observability story (200+ atomic counters, perf CSV pipeline, SLO scoring) is far beyond what most ESP32 projects achieve. The bones are strong. What follows is an honest accounting of where things shine and where the rough edges are.

---

## 1. Architecture Conformance

**Rating: 9/10**

The `ARCHITECTURE.md` lays out two wiring patterns — `begin()` pointer injection (default) and `Providers` struct with `void* ctx` function pointers (for testable modules). The codebase follows this with remarkable consistency:

- 41 modules use `begin()` for dependency injection
- 14 system-loop modules use the `Providers` pattern
- Zero instances of `#include "main_globals.h"` inside `src/modules/`
- Zero `extern` declarations inside module files
- All private members use trailing underscore (`settings_`, `ble_`, `state_`)
- All module classes follow PascalCase + `Module`/`Service` suffix naming

**Known violations (documented and accepted):** 11 files use `std::function` for callback wiring, all in WiFi API service handlers — lower-priority, non-hot-path code. The architecture doc flags `std::function` as retired for module wiring but these are lifecycle setup callbacks, not runtime wiring. Acceptable, but should be migrated when those files are next touched.

**One genuine concern:** `main.cpp` declares 20+ global module instances. This is unavoidable on ESP32 (no OS-level DI container), and `main_globals.h` correctly uses forward declarations to limit compile coupling. But `mainRuntimeState` is directly mutated in `loop()` across phases, creating implicit ordering dependencies. The `RUNTIME_OWNERSHIP.md` document exists precisely to track this — good discipline, but fragile if someone doesn't read it.

---

## 2. Priority Order Compliance

**Rating: 10/10**

The priority stack from the instructions (`V1 connectivity > BLE ingest > Display > Audio > Metrics > WiFi > Logging`) is correctly reflected in the loop phase sequence:

1. `processLoopConnectionEarlyPhase` — BLE connection decisions
2. `processLoopIngestPhase` — BLE queue drain, GPS refresh
3. `loop()` body — OBD/speed arbitration
4. `processLoopDisplayPreWifiPhase` — display + lockout (never blocks BLE)
5. `processLoopWifiPhase` — WiFi progression (deferred until after display)
6. `processLoopFinalizePhase` — telemetry, persistence (best-effort)

WiFi is truly non-blocking with a 6-stage STA connect pipeline and 5-stage teardown, each yielding between stages. Display throttles at 10 FPS max. Persistence uses try-lock with non-blocking fallback. The priority model isn't just documented — it's enforced in code.

---

## 3. BLE Subsystem

**Rating: 8.5/10**

Strengths: Non-blocking async state machine for connect/discover/subscribe. Exponential backoff (200ms→1.5s) with hard reset after 5 failures. Bond backup/restore to SD. Callback path is latency-safe — atomic flags for deferred work, try-lock mutexes with timeout=0, RAII semaphore guards. Proxy queue uses PSRAM with internal SRAM fallback. Buffer bounds are checked everywhere.

Issues found:

- **Pacing race condition** (`ble_commands.cpp`): `static unsigned long lastCommandMs` is not mutex-protected. Two concurrent calls could bypass pacing. Low impact in single-loop design but technically unsound.
- **Characteristic lookup fallback** (`ble_connection.cpp`): If both primary and alt command characteristics fail `canWrite()`, `pCommandChar` could hold a stale value from a previous session. Should explicitly nullptr and fail the step.
- **`forwardToProxyImmediate()` is misleading**: Named to suggest zero-latency lock-free path, but just calls `forwardToProxy()` which acquires a mutex. Should be renamed or made truly lock-free.
- **Magic UUIDs duplicated** across `ble_connection.cpp` and `ble_proxy.cpp`. Should be centralized constants.
- **Double backslash** in `Serial.printf` at `packet_parser.cpp:175` — prints literal `\n` instead of newline.

---

## 4. Display Subsystem

**Rating: 7.5/10 (your identified weak area — confirmed, but better than expected)**

The display code is actually well-structured: 12 CPP files totaling ~4,500 LOC with clear separation (frequency rendering, bands, cards, arrows, status bar, sliders, indicators). The dirty flag system is three-layered (aggregate flags → frequency dirty region → per-renderer caches) and prevents unnecessary redraws effectively. Font rendering uses OpenFontRender with PSRAM-aware cache sizing (49KB with PSRAM, 8KB without) and glyph prewarming at boot.

Where it falls short:

- **Row-by-row flush** in `flushRegion()`: Iterates `h` times calling `draw16bitRGBBitmap()` for one row each. For a 20-row region, that's 20 separate SPI transactions. Batching rows would reduce overhead significantly.
- **Serpentine lazy-load latency**: First Serpentine-style frequency render blocks while the font loads and first glyph rasterizes. No background loading.
- **No error checking on OFR glyph cache fills**: If glyph rasterization fails under memory pressure, there's no graceful fallback mid-render.
- **No test coverage for rendering**: Display *modules* (orchestration, pipeline, restore) are well-tested, but actual GFX drawing code has zero unit tests. The pixel-level output, text positioning, color application, and DMA timing are entirely validated by eye. This is the biggest gap.
- **220KB framebuffer** in PSRAM (172×640×2 bytes) — appropriate for the hardware, but the single-buffer architecture means micro-tearing is possible between row flushes. Not likely noticeable on AMOLED, but worth documenting.

---

## 5. Test Suites

**Rating: 7/10 (your identified weak area — confirmed)**

The numbers are impressive: 137 test directories, ~1,400 tests passing, ~1m39s runtime, plus ASan/UBSan nightly, mutation testing on critical paths, and a 22-step semantic CI gate. The test infrastructure is sophisticated. But coverage has real gaps.

**Well-covered:**
- Lockout subsystem (9 suites, mutation testing, area safety boundary tests)
- WiFi (20 suites including boot policy, orchestration, all API services)
- BLE (9 suites: connection states, bond backup, fresh flash policy)
- Settings/persistence (NVS, backup, deferred persist, rollback)
- Packet parsing (frame validation, streams, alert assembly)

**Under-covered:**
- **Display rendering** — no tests for any `display_*.cpp` GFX routines
- **Boot sequence** — only hardware device tests, no native unit tests for `main_boot.cpp` or `main_setup_helpers.cpp`
- **Storage failure scenarios** — mocks don't simulate SD card full, NVS corruption, concurrent SD mutex contention
- **BLE protocol edge cases** — state binding tested but discovery timeouts, subscription failures, and connect/disconnect races not exercised
- **Time service** — no dedicated tests at all
- **Battery/ADC** — one suite, no sensor noise or low-battery edge cases

**Mock quality issues:**
- Mocks don't support fault injection (I2C failures, SD errors)
- BLE mock doesn't simulate connection drops or MTU negotiation
- Display mock doesn't verify pixel output
- WiFi mock doesn't simulate scan timeouts or weak signal
- No concurrency/FreeRTOS scheduling tests

**Structural issues:**
- Many tests use file-scoped static globals (`static LockoutIndex testIndex`) reset in `setUp()`. If a test fails mid-execution, state can leak to subsequent tests. Low risk since tests run serially, but brittle.
- Some tests have empty `tearDown()` — usually fine but adds cognitive load about what cleanup is expected.
- Large `setUp()` blocks lack fixture/helper abstractions.

---

## 6. WiFi Manager

**Rating: 8.5/10**

Truly non-blocking staged state machines for both STA connect and disconnect. DMA heap starvation detection prevents SD and WiFi from starving each other. Rate limiting on all HTTP routes. AP idle retirement after 60s when STA connected. Memory overhead under 1KB for the manager itself.

Issues:

- **Silent reconnect failure**: After 5 STA reconnect failures, WiFi silently gives up. No user notification mechanism (no display indicator, no audio alert).
- **`std::function` callbacks** (9 instances in `wifi_manager.h`): Architecture doc says these should be migrated to pointer injection. They're lifecycle callbacks, not hot-path, but it's tech debt.
- **WiFi client credentials** use XOR obfuscation (not encryption). Documented and accepted for the use case, but worth calling out.

---

## 7. Settings & Storage

**Rating: 9/10**

Dual-namespace NVS with health scoring, atomic clear-and-rewrite, SD backup with temp→validate→promote→cleanup pipeline, deferred persistence with 750ms debounce. This is robust.

Minor concerns:

- **JsonDocument stack allocation in `main_persist.cpp`**: Creates new `JsonDocument` every 15 seconds in the learner save path. Should be pre-allocated or static.
- **No NVS write performance telemetry**: You track SD write drops but not NVS write latency or namespace recovery activations.
- **Deferred persist retry** uses flat 1-second backoff (no exponential). If NVS is consistently failing, it'll hammer retries.

---

## 8. Performance Metrics & Observability

**Rating: 9.5/10**

This is the showcase feature. 200+ atomic counters with `memory_order_relaxed` (zero runtime overhead), perf CSV logging to SD with session markers, deterministic SLO scoring tool with hard/advisory thresholds, week-over-week trend comparison, and CI contract guards to keep the doc and JSON thresholds in sync.

The only gap: no histogram for SD write latency itself, and no NVS timing data. You measure everything except the cost of measuring.

---

## 9. CI/CD Pipeline

**Rating: 9/10**

The `ci-test.sh` script is authoritative and thorough: 22 semantic guards (BLE deletion safety, display flush discipline, SD lock semantics, main loop call order), native unit tests, functional scenarios, critical mutation gate, perf scorer regression tests, and compatibility contract checks. Five GitHub workflows cover build, nightly validation, pre-release validation, release-on-merge, and stability trending.

The semantic guard approach (Python scripts that grep/analyze source for invariant violations) is clever and effective. The mutation testing on critical paths (`critical_mutations.json`) is a mature practice rarely seen in embedded projects.

---

## 10. "First Project Mess-Ups"

Things that betray the learning curve:

1. **`compile_commands.json` and `node_modules/` checked into the repo** — these should be gitignored
2. **`.road_map_cache/` with binary JSON chunks in the repo root** — should be gitignored or generated
3. **`.scratch/` directory with vendored Android ESP library source** — should be a submodule or removed
4. **`.venv/` in the repo** — should be gitignored
5. **`road_map.bin` binary in repo root** — large binary should be in releases or a data artifact
6. **Hardcoded local paths** in docs (`/Users/ajmedford/v1g2_simple/...`) in `PERF_SLOS.md` and `RUNTIME_OWNERSHIP.md` — should be relative paths
7. **Two dead wrapper functions** in `main.cpp`: `isColorPreviewRunning()` and `cancelColorPreview()` — trivially removable

---

## 11. Summary: Strengths and Action Items

### What's genuinely strong

- Architecture discipline (DI everywhere, no globals in modules, documented ownership)
- Priority model actually enforced in code, not just documented
- BLE non-blocking state machine with proper backoff and bond management
- Observability story (200+ metrics, SLO scoring, trend comparison)
- CI pipeline with semantic guards and mutation testing
- Settings corruption protection with health-scored recovery
- WiFi truly non-blocking with DMA starvation protection

### Where to focus next (ordered by impact)

1. **Display rendering tests** — biggest coverage gap. Even basic "does drawBands() set expected pixels" tests would catch regressions.
2. **Mock fault injection** — add error-returning paths to storage, I2C, and BLE mocks. Critical for "bulletproof" aspiration.
3. **Boot sequence native tests** — `main_boot.cpp` and `main_setup_helpers.cpp` have no native test coverage.
4. **Fix repo hygiene** — gitignore `node_modules`, `.venv`, `.road_map_cache`, `compile_commands.json`, `road_map.bin`.
5. **Migrate WiFi `std::function` callbacks** — match the architecture doc.
6. **Pre-allocate JsonDocument in persist path** — avoid repeated heap allocation every 15s.
7. **Centralize BLE UUIDs** — single source of truth for V1 protocol constants.
8. **Add STA reconnect failure notification** — surface to user via display or audio.
9. **Fix hardcoded paths in docs** — use relative paths.
10. **Add SD write latency histogram** — close the observability gap.

---

*This review is read-only. No changes were made to the codebase.*
