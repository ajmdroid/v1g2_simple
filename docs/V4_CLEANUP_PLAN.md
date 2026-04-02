# V4 Cleanup Plan — Finishing the Module Extraction

> Post-mortem on the v3→v4 refactor (625 commits, 15 modules, ~17,500 lines extracted).
> The refactor succeeded structurally but left loose threads. This plan closes them.
>
> **Approach**: Conservative — zero-risk fixes first, build confidence, then escalate.
> **Testing**: Hybrid — new tests for risky changes (display), existing suite for safe fixes.

---

## Summary of Findings

The v4 module extraction achieved its primary goals: `main.cpp` is clean orchestration
(41 modules, 19 wiring functions, ~100-line loop), 73+ modules follow proper DI patterns,
and the `std::function` migration is fully complete (contrary to stale docs that say
otherwise).

Five categories of loose threads remain:

| # | Category | Risk | Effort | Status |
|---|----------|------|--------|--------|
| 1 | Stale documentation | None | ~30 min | Docs say std::function migration is pending — it's done |
| 2 | Dead display reset functions | Low | ~1 hour | 8 functions declared, defined, never called |
| 3 | Display dirty flag gap | Medium | ~2 hours | 30+ file-scoped statics operate as parallel cache layer |
| 4 | Global-access violations | Medium | ~4 hours | 3 modules bypass DI, access globals directly |
| 5 | Known display defects | Low-Med | ~2 hours | Integer overflow, RGB565 validation, flushRegion (mitigated) |

---

## Phase 1 — Zero-Risk Fixes (Day 1)

**Goal**: Ship confidence. Every change here is documentation or dead code removal.
Run `pio test -e native` and `scripts/ci-test.sh` after each step to confirm green.

### 1.1 Update ARCHITECTURE.md — std::function Section

The "Retired" section (lines 106–121) says:

> "Existing uses in the WiFi API services are to be migrated to direct pointer
> injection when those files are next touched."

**This work is done.** Every WiFi API service now uses C function pointers with
`void* ctx`. The only remaining `std::function` instances are:

- `test/mocks/NimBLEDevice.h:126` — mimics the NimBLE library interface (correct)
- `test/test_wifi_status_api_service/test_wifi_status_api_service.cpp:55-56` — test helper (acceptable)

**Action**: Update the Retired section to reflect completion. Change language from
"are to be migrated" to "have been migrated" and note the two acceptable test-only
exceptions.

**Files**: `docs/ARCHITECTURE.md` (lines 106–121)
**Tests**: Existing CI (no code change)
**Risk**: Zero

### 1.2 Remove Dead Display Reset Functions

Eight functions are declared, defined, and **never called from any code path**:

| Function | File | Line |
|----------|------|------|
| `resetBandsCache()` | `src/display_bands.cpp` | 134 |
| `resetSignalBarsCache()` | `src/display_bands.cpp` | 217 |
| `resetArrowCache()` | `src/display_arrow.cpp` | 304 |
| `resetIndicatorsCache()` | `src/display_indicators.cpp` | 132 |
| `resetStatusBarCache()` | `src/display_status_bar.cpp` | 564 |
| `resetFrequencyCache()` | `src/display_frequency.cpp` | 383 |
| `resetTopCounterCache()` | `src/display_top_counter.cpp` | 430 |
| `resetCardsCache()` | `src/display_cards.cpp` | 467 |

Also remove their declarations from `src/display.h` (lines ~77–84).

**Why these are truly dead**: The actual cache invalidation goes through a different
path entirely:

```
resetChangeTracking()
  → dirty.resetTracking = true
    → DisplayRenderCache::resetRestingTracking() / resetLiveTracking()
      → resets high-level state tracking in display_update.cpp
        → next frame sees stale values → triggers full redraw
```

The per-element dirty flag pattern (`if (dirty.bands) { s_bandCacheValid = false; }`)
handles the element-level caches. The `resetXxxCache()` functions were written as part
of the migration plan but never wired in — the dirty flag approach was used instead.

**Action**: Delete all 8 functions and their declarations. Verify no call sites exist
(grep confirms zero callers).

**Files**: `src/display.h`, `src/display_bands.cpp`, `src/display_arrow.cpp`,
`src/display_indicators.cpp`, `src/display_status_bar.cpp`, `src/display_frequency.cpp`,
`src/display_top_counter.cpp`, `src/display_cards.cpp`
**Tests**: Existing CI (dead code removal)
**Risk**: Zero — no code path references these functions

---

## Phase 2 — Known Defect Fixes (Day 2–3)

**Goal**: Close the documented defects from CLAUDE.md. These are targeted, well-understood
fixes in code we've already audited.

### 2.1 Fix Integer Overflow in Dirty Region Union (display_frequency.cpp)

**Location**: `src/display_frequency.cpp:85-86`

**Current code**:
```cpp
const int16_t x2 = max(
    static_cast<int16_t>(static_cast<int32_t>(frequencyDirtyX_) + static_cast<int32_t>(frequencyDirtyW_)),
    static_cast<int16_t>(static_cast<int32_t>(x) + static_cast<int32_t>(w))
);
```

**Problem**: Addition is done in `int32_t` but immediately cast down to `int16_t`.
If the sum exceeds 32767, silent truncation. On a 640-wide display this is unlikely
but not impossible if dirty regions accumulate across layout changes.

**Fix**: Keep the arithmetic in `int32_t` and clamp to display bounds before casting:

```cpp
const int32_t x2_raw = max(
    static_cast<int32_t>(frequencyDirtyX_) + static_cast<int32_t>(frequencyDirtyW_),
    static_cast<int32_t>(x) + static_cast<int32_t>(w)
);
const int16_t x2 = static_cast<int16_t>(min(x2_raw, static_cast<int32_t>(DISPLAY_WIDTH)));
```

**Files**: `src/display_frequency.cpp`
**Tests**: Write a new test in `test/test_display_frequency/` covering:
  - Normal union (both regions fit in display)
  - Edge case: region extends to display boundary
  - Overflow case: sum exceeds int16_t max (artificial, but proves the clamp works)
**Risk**: Low — targeted arithmetic fix with no behavioral change in normal operation

### 2.2 Add RGB565 Color Validation

**Problem**: Colors loaded from NVS settings are used directly without validation.
A corrupt byte in flash produces garbage colors with no fallback.

**Locations**: `src/display.cpp:398-407` (getBandColor) and any other settings-to-color
paths.

**Fix**: Add a validation helper that range-checks RGB565 values and falls back to
the default palette color on failure. RGB565 is a 16-bit value — any `uint16_t` is
technically valid, so the real concern is `0x0000` (black on black background) or
values that match the background color, making elements invisible.

More practically: add a "not background color" check and a "not zero unless
intentional" guard. The existing default palette colors in `include/color_themes.h`
provide known-good fallbacks.

**Files**: `src/display.cpp`, potentially `include/display_helpers.h` for the validator
**Tests**: Existing suite (behavioral change is only in corrupt-settings edge case)
**Risk**: Low

### 2.3 Close flushRegion Null Pointer Defect

**Status**: Already mitigated with null checks at `display.cpp:332-349`.

**Action**: Remove from the known-defects list in CLAUDE.md. Add a brief comment at
the mitigation site documenting that this was a tracked defect and is resolved.

**Files**: `CLAUDE.md`, `src/display.cpp` (comment only)
**Tests**: None needed
**Risk**: Zero

---

## Phase 3 — DI Violations (Day 4–6)

**Goal**: Bring the three global-accessing modules into architectural compliance.
Each gets its own commit so regressions are bisectable.

### 3.1 Inject Dependencies into backup_api_service

**Current state**: Accesses `settingsManager` (4 calls), `storageManager` (3 calls),
and `v1ProfileManager` (via BackupPayloadBuilder) directly from inside
`src/modules/wifi/backup_api_service.cpp`.

**Approach**: Follow the established pattern from `wifi_settings_api_service`:

1. Define a `BackupRuntime` struct in `backup_api_service.h` with C function pointers:
   - `getBackupRevision`, `getSettings`, `applyBackupDocument`, `backupToSD`
   - `getCatalogRevision` (for v1ProfileManager)
   - `isStorageReady`, `isSDCard` (for storageManager)
   - Each with paired `void* ctx`

2. Update handler signatures to accept `const BackupRuntime&`

3. Add `makeBackupRuntime()` factory in `src/wifi_runtimes.cpp` that binds globals

4. Update route registration in `src/wifi_routes.cpp`

**Scope**: ~100-150 lines across 5-6 files. Pattern is identical to existing services.

**Files**: `src/modules/wifi/backup_api_service.h`, `src/modules/wifi/backup_api_service.cpp`,
`src/wifi_runtimes.cpp`, `src/wifi_routes.cpp`, `src/wifi_manager.h`
**Tests**: Update `test/test_backup_api_service/` to mock the Runtime struct (cleaner
than current approach of globally overriding functions)
**Risk**: Medium — changes API service wiring, but pattern is proven

### 3.2 Inject Dependencies into debug_perf_files_service

**Current state**: 12+ direct accesses to `storageManager` and 4+ to `perfSdLogger`
scattered across `src/modules/debug/debug_perf_files_service.cpp`.

**This is the messiest violation.** The file uses `storageManager.getSDMutex()`,
`storageManager.getFilesystem()`, `perfSdLogger.csvPath()` etc. — deep coupling
to storage internals.

**Approach**: Same Runtime struct pattern, but the struct will be larger because
the file touches more storage surface area:
- `isReady`, `isSDCard`, `getFilesystem`, `getSDMutex` (from storageManager)
- `isEnabled`, `csvPath` (from perfSdLogger)

**Files**: `src/modules/debug/debug_perf_files_service.h`,
`src/modules/debug/debug_perf_files_service.cpp`, wiring in main.cpp or WiFi setup
**Tests**: Write new tests for the service using mock Runtime (currently untested —
this is an opportunity to add coverage)
**Risk**: Medium — more touch points than backup, but non-critical path (debug only)

### 3.3 Clean Up debug_api_service

**Current state**: Uses a `DebugApiService::deps` namespace with extern pointers
that are assigned via `begin()`. This is architecturally halfway — it has injection
discipline but uses extern declarations to store the pointers.

**Approach**: Migrate the `deps` namespace to a proper `Providers` or `Runtime`
struct. This is the smallest change of the three since the injection discipline
already exists.

**Files**: `src/modules/debug/debug_api_service.cpp`,
`src/modules/debug/debug_api_service_deps.inc`, header
**Tests**: Existing suite
**Risk**: Low — the injection pattern is already in place, just needs a container change

---

## Phase 4 — Display Cache Architecture Review (Day 7–8, Planning Only)

**Goal**: Don't touch the display cache layer yet. Instead, document the current
two-layer architecture clearly so future work is informed.

### Why Not Fix It Now

The display rendering pipeline works. The two-layer cache system (dirty flags +
file-scoped statics) is redundant but not broken:

- The dirty flags signal invalidation
- The file-scoped statics cache the last-rendered values
- Both must agree for a skip-redraw to happen
- The dirty flags are conservative (they over-invalidate, not under-invalidate)

Unifying these into a single layer is a significant refactor that touches every
`display_*.cpp` file simultaneously. It should be done, but not during a cleanup
pass where the goal is closing known gaps safely.

### What To Do Instead

1. **Document the two-layer architecture** in a new section of `docs/ARCHITECTURE.md`:
   explain how dirty flags and file-scoped statics interact, why both exist, and
   what a future unification would look like.

2. **Add a contract test** that verifies every `display_*.cpp` file that reads a
   dirty flag also resets it in the same function. This catches the most dangerous
   failure mode (flag consumed but not cleared → permanent redraw loop or permanent
   stale cache).

3. **File a tracking item** for the eventual unification. The work is:
   - Move all `s_*` statics into a `DisplayElementCache` struct
   - Have each dirty flag reset function zero the corresponding cache section
   - Remove the per-function `if (dirty.x) { s_xCacheValid = false; }` pattern
   - Replace with centralized invalidation in `DisplayDirtyFlags::setAll()`

**Files**: `docs/ARCHITECTURE.md` (new section)
**Tests**: New contract script in `scripts/`
**Risk**: Zero — documentation and safety net only

---

## Verification After Each Phase

After completing each phase:

```bash
# Unit tests
pio test -e native

# Full CI contract suite
scripts/ci-test.sh

# Firmware compiles clean
pio run -e waveshare-349
```

All three must pass before moving to the next phase.

---

## Success Criteria

When this plan is complete:

- [ ] ARCHITECTURE.md accurately reflects the current state of the codebase
- [ ] Zero dead code in the display rendering pipeline
- [ ] All three known defects in CLAUDE.md are resolved or documented as resolved
- [ ] Zero architecture violations in `src/modules/` (all globals injected via DI)
- [ ] Display cache architecture is documented with a contract test guarding it
- [ ] Every change is bisectable (one concern per commit)

---

## What This Plan Deliberately Excludes

- **Display cache unification**: Too risky for a cleanup pass. Documented and
  contracted instead.
- **New features**: This is pure debt paydown. No new capabilities.
- **Performance work**: The perf SLO system is mature and passing. Don't touch it.
- **Web UI changes**: The Svelte frontend is stable and separately tested.

---

*Created: 2026-04-02*
*Context: First-week codebase audit of v4.0.0 release*
