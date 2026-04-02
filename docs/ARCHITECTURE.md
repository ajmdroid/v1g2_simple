# Architecture & Code Ideology

Reference document for module design, wiring patterns, and code quality
requirements in the V1-Simple firmware.

---

## Core Principle

Every module owns its logic and nothing else. It does not reach for globals,
does not know who wired it, and does not care where its dependencies come from.
Dependencies are always injected — never fetched.

---

## Header Placement

| Directory | Purpose |
|---|---|
| `include/` | Headers consumed by multiple subsystems or needed by tests |
| `src/*.h` | Private headers for a single `.cpp` translation unit |
| `src/modules/<category>/` | Module-scoped headers (co-located with implementation) |

There is no `src/include/` directory. Do not create one.

---

## Module Wiring: The Canonical Pattern

### Default — Direct Pointer Injection via `begin()`

Use this for all modules. Dependencies are passed as typed pointers at
startup via a `begin()` call. Pointers are stored as private members and
used for the lifetime of the module.

```cpp
// alert_persistence.h
class AlertPersistence {
public:
    void begin(const SettingsManager* settings);

    bool shouldKeepAlert(uint32_t nowMs, uint32_t clearedAtMs) const;
private:
    const SettingsManager* settings_ = nullptr;
};
```

**Rules:**
- All pointers passed to `begin()` must remain valid for the module's lifetime.
- All pointer members are initialized to `nullptr` and only populated in `begin()`.
- `begin()` is called once at startup. Never called again unless the module is fully reset.
- `process()` and other methods assert (or guard) that `begin()` was called if needed.
- No default arguments that hide required dependencies. Optional dependencies
  (e.g. `store`) are the exception and must be documented.

---

### Exception — `Providers` Struct + `void* ctx` Function Pointers

Use this **only** when the module has unit tests that need to run without
linking real firmware dependencies. The `Providers` pattern creates an
explicit seam that can be satisfied by a test double without instantiating
any real subsystem.

```cpp
// loop_tail_module.h
class LoopTailModule {
public:
    struct Providers {
        uint32_t (*perfTimestampUs)(void* ctx) = nullptr;
        void*     perfTimestampContext          = nullptr;

        void (*runBleDrain)(void* ctx) = nullptr;
        void*  bleDrainContext         = nullptr;

        void (*yieldOneTick)(void* ctx) = nullptr;
        void*  yieldContext             = nullptr;
    };

    void begin(const Providers& hooks);
    uint32_t process(bool bleBackpressure, uint32_t loopStartUs);

private:
    Providers providers{};
};
```

Use `ProviderCallbackBindings::member<T, &T::method>` at the call site in
`main.cpp` to bind real implementations cleanly:

```cpp
providers.runBleDrain =
    ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
providers.bleDrainContext = &bleQueueModule;
```

**Rules:**
- Only adopt this pattern if the module has or will have native unit tests.
- Every function pointer has a paired `void* ctx`. No raw captures, no globals.
- All function pointer members default to `nullptr`.
- The module never calls a provider without checking it is non-null (or the
  design guarantees it via `begin()` validation).

---

### Retired — `std::function` Runtime Structs

`std::function` is **not used** for module wiring in this codebase.

It was previously used in the WiFi API services but carried heap allocation
overhead that is unacceptable on ESP32. All WiFi API services have been
migrated to C function pointers with paired `void* ctx` (the Providers
pattern). This migration is complete as of v4.0.0.

The only remaining `std::function` instances are in test infrastructure
(NimBLE library mock interface and one test helper struct), which is
acceptable — test code does not run on the ESP32.

```cpp
// DO NOT DO THIS — legacy pattern, fully retired
struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<void()> save;
};
```

---

## Decision Rule

```
Does this module have (or need) unit tests?
    YES → Providers struct pattern
    NO  → begin() direct pointer injection
```

If you are unsure, default to `begin()`. Adding a `Providers` seam later
when tests are written is straightforward. Adding tests to a `begin()`
module is also possible via thin test wrappers if the module itself is
simple enough.

---

## Global Variables

Global module instances are declared in `main.cpp` and `src/` top-level
files only. `src/modules/` files must not reference globals directly.

**Prohibited inside `src/modules/`:**
```cpp
// DO NOT DO THIS inside any modules/ file
extern SettingsManager settingsManager;
settingsManager.get().enableWifi;
```

**Required:**
- All dependencies enter a module through `begin()` or `Providers`.
- `main_globals.h` is not included by any file under `src/modules/`.
- As of v4.0.0, all modules under `src/modules/` comply with DI requirements.
  `backup_api_service`, `debug_perf_files_service`, and `debug_api_service`
  were migrated to Runtime/Providers structs with C function pointers during
  the v4 cleanup.

---

## Module Structure Requirements

### Header

```cpp
#pragma once

// Forward-declare all dependencies — do not include full headers
// unless the type must be complete (e.g. embedded by value, not pointer).
class SettingsManager;
class ObdRuntimeModule;

class MyModule {
public:
    /// Wire dependencies. Must be called once before process().
    /// All pointers must remain valid for the lifetime of this module.
    void begin(SettingsManager* settings, ObdRuntimeModule* obd);

    /// Brief description of what process() does and when to call it.
    void process(uint32_t nowMs);

private:
    SettingsManager*    settings_ = nullptr;
    ObdRuntimeModule*   obd_      = nullptr;
};
```

- Use forward declarations in headers. Include full headers in `.cpp` only.
- Private members use trailing underscore (`settings_`, `index_`).
- Public API is minimal — expose only what callers need.
- Document `begin()` parameters, especially lifetime expectations.

### Implementation

```cpp
#include "my_module.h"
#include "settings.h"       // Full include in .cpp is fine
#include "obd_runtime_module.h"

void MyModule::begin(SettingsManager* settings, ObdRuntimeModule* obd) {
    settings_ = settings;
    obd_      = obd;
}

void MyModule::process(uint32_t nowMs) {
    // Guard is optional but recommended for debug builds
    // settings_ and obd_ are guaranteed non-null by contract
}
```

---

## Naming Conventions

| Thing | Convention | Example |
|---|---|---|
| Module class | `PascalCase` + `Module` suffix | `DisplayOrchestrationModule` |
| Service (stateless namespace) | `PascalCase` + `Service` suffix | `WifiSettingsApiService` |
| Private member | trailing underscore | `settings_`, `index_` |
| `begin()` parameter | no prefix/suffix | `settings`, `index` |
| Result struct | `<Module>Result` | `DisplayOrchestrationResult` |
| Providers struct | nested `Providers` inside class | `LoopTailModule::Providers` |

---

## What a Good Module Looks Like

- Has one clear responsibility described in one sentence.
- All dependencies arrive through `begin()` or `Providers`. Zero globals.
- Header uses forward declarations only.
- Private state uses trailing underscore naming.
- `process()` is cheap to call — no dynamic allocation, no blocking I/O.
- If it has state, that state is fully reset by a `reset()` method.
- If it has unit tests, it uses the `Providers` pattern. Otherwise `begin()`.

---

## What Triggers a Review Flag

- Any `#include "main_globals.h"` inside `src/modules/`.
- Any `std::function` used for module wiring.
- Any `extern` declaration inside a module file.
- A `begin()` that takes more than ~6 parameters without a strong reason.
- A module whose `process()` allocates heap memory.
- A module that calls into another module without that module being
  injected through `begin()`.

---

## Display Cache Architecture — Two-Layer Invalidation

The display rendering pipeline uses a two-layer cache system to minimize
SPI redraws on the ESP32-S3. Both layers must agree for a redraw to be
skipped. This section documents how they interact and why both exist.

### Layer 1 — DisplayDirtyFlags (Coarse Invalidation Signals)

Defined in `include/display_dirty_flags.h`, this is a flat struct of booleans:

```cpp
struct DisplayDirtyFlags {
    bool multiAlert    = false;
    bool cards         = false;
    bool frequency     = false;
    bool battery       = false;
    bool bands         = false;
    bool signalBars    = false;
    bool arrow         = false;
    bool muteIcon      = false;
    bool topCounter    = false;
    bool obdIndicator  = false;
    bool resetTracking = false;   // Full cache reset signal
};
```

A global `dirty` instance lives in `display.cpp`. Any subsystem can set a
flag to force a redraw of the corresponding element on the next frame.
`setAll()` marks every primary element dirty (used after screen clear).

Flags are **conservative** — they over-invalidate, never under-invalidate.
Setting `dirty.bands = true` forces a band redraw even if the rendered
state hasn't actually changed. This is safe (wastes one frame of SPI
bandwidth) but never causes stale output.

### Layer 2 — File-Scoped Static Caches (Fine-Grained Change Detection)

Each `display_*.cpp` file maintains `static` variables that record the
exact values drawn on the last frame. These enable incremental updates:
a function can skip the redraw entirely if the dirty flag is clear **and**
the current values match the cached last-drawn values.

Representative examples:

| File | Key statics | What they track |
|---|---|---|
| `display_bands.cpp` | `s_bandLastEffectiveMask`, `s_bandCacheValid` | Which band indicators were drawn |
| `display_arrow.cpp` | `s_arrowLastShowFront/Side/Rear`, `s_arrowCacheValid` | Arrow direction state |
| `display_frequency.cpp` | `s_freqClassicLastText`, `s_freqClassicCacheValid` | Frequency text and color |
| `display_status_bar.cpp` | `s_batteryLastPctDrawn`, `s_batteryLastPctColor` | Battery percentage and color |
| `display_top_counter.cpp` | `s_topCounterLastSymbol`, `s_topCounterLastMuted` | Bogey counter symbol |
| `display_cards.cpp` | `s_cardsLastDrawnPositions[]`, `s_cardsLastDrawnCount` | Secondary alert card layout |
| `display_indicators.cpp` | `s_obdLastShown`, `s_obdLastConnected` | OBD indicator visibility |

### How The Two Layers Interact

When a dirty flag fires, the corresponding render function invalidates
its local cache and redraws unconditionally:

```cpp
// display_bands.cpp — representative pattern
if (dirty.bands) {
    s_bandCacheValid = false;    // Invalidate local cache
    dirty.bands = false;         // Consume the flag
}

// Later in the same function:
if (s_bandCacheValid && mask == s_bandLastEffectiveMask && ...) {
    return;  // Skip — nothing changed since last draw
}

// ... perform the actual draw ...
s_bandCacheValid = true;
s_bandLastEffectiveMask = mask;
```

The dirty flag is always consumed (set to `false`) in the same function
that reads it. This prevents permanent redraw loops. The local cache is
reset to force at least one full redraw, after which incremental
change-detection takes over again.

### DisplayRenderCache — Cross-Element State

`display_update.cpp` contains a `DisplayRenderCache` struct (`s_displayRenderCache`)
that tracks state spanning multiple elements across resting and live alert
modes. It records the last-rendered priority alert, bogey byte, arrow state,
volume levels, and mode-specific first-run flags.

Two reset methods handle mode transitions:

- `resetRestingTracking()` — clears resting-mode caches (band debounce,
  signal bars, arrows, volume, bogey counter)
- `resetLiveTracking()` — clears live-mode caches (priority alert, arrows,
  bars, bands, volume, multi-alert state)

These are triggered by `dirty.resetTracking`, which is set by
`V1Display::resetChangeTracking()` on BLE disconnect or forced redraw.

### Why Both Layers Exist

The dirty flags alone are insufficient because they don't carry enough
information. Knowing that `dirty.bands = true` tells you *something*
changed, but not *what*. The render function still needs to compare the
current state against what was last drawn to produce the correct output.

The file-scoped statics alone are insufficient because some invalidation
comes from *outside* the data that the render function tracks. A screen
mode change, BLE reconnect, or settings update may require a full redraw
even though the alert data hasn't changed. The dirty flag forces the
render function past its "nothing changed" short-circuit.

Together they achieve efficient incremental rendering:

1. Dirty flag fires → local cache invalidated → full element redraw
2. Dirty flag clear + values unchanged → skip (zero SPI cost)
3. Dirty flag clear + values changed → incremental update (minimal SPI cost)

### Dirty Region Optimization

`display_frequency.cpp` additionally implements partial dirty region
tracking (`frequencyDirtyX_`, `frequencyDirtyY_`, etc.) to push only
changed pixels via `flushRegion()`. This minimizes SPI bandwidth for
the largest display element (the frequency readout).

### Contract: Flag Consumption Rule

**Every function that reads a dirty flag must clear it before returning.**
If a flag is consumed but not cleared, one of two failure modes results:

- **Permanent redraw loop**: the flag stays `true` forever, forcing a
  full redraw every frame (wastes SPI bandwidth, may cause visible flicker)
- **Stale cache**: if the flag is cleared without invalidating the local
  cache, changes may never render

The `scripts/check_dirty_flag_discipline.sh` contract test enforces this
rule at CI time.

### Future: Cache Unification

The two-layer system works correctly but is redundant. A future refactor
could unify them:

1. Move all `s_*` statics into a `DisplayElementCache` struct
2. Have each dirty flag setter zero the corresponding cache section
3. Remove the per-function `if (dirty.x) { s_xCacheValid = false; }` pattern
4. Replace with centralized invalidation in `DisplayDirtyFlags::setAll()`

This is a significant refactor touching every `display_*.cpp` file and
should be done as a dedicated effort with comprehensive test coverage,
not during a cleanup pass.
