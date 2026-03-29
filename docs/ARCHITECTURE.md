# Architecture & Code Ideology

Reference document for module design, wiring patterns, and code quality
requirements in the V1-Simple firmware.

---

## Core Principle

Every module owns its logic and nothing else. It does not reach for globals,
does not know who wired it, and does not care where its dependencies come from.
Dependencies are always injected — never fetched.

---

## Module Wiring: The Canonical Pattern

### Default — Direct Pointer Injection via `begin()`

Use this for all modules. Dependencies are passed as typed pointers at
startup via a `begin()` call. Pointers are stored as private members and
used for the lifetime of the module.

```cpp
// lockout_enforcer.h
class LockoutEnforcer {
public:
    void begin(const SettingsManager* settings,
               LockoutIndex* index,
               LockoutStore* store = nullptr);

    LockoutEnforcerResult process(uint32_t nowMs, const PacketParser& parser,
                                  const GpsRuntimeStatus& gps);
private:
    const SettingsManager* settings_ = nullptr;
    LockoutIndex*          index_    = nullptr;
    LockoutStore*          store_    = nullptr;
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

It was introduced in the WiFi API services and carries heap allocation
overhead that is unacceptable on ESP32. Existing uses in the WiFi API
services are to be migrated to direct pointer injection when those files
are next touched.

```cpp
// DO NOT DO THIS
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
- The debug API services are a known violation and are flagged for cleanup.

---

## Module Structure Requirements

### Header

```cpp
#pragma once

// Forward-declare all dependencies — do not include full headers
// unless the type must be complete (e.g. embedded by value, not pointer).
class SettingsManager;
class LockoutIndex;

class MyModule {
public:
    /// Wire dependencies. Must be called once before process().
    /// All pointers must remain valid for the lifetime of this module.
    void begin(SettingsManager* settings, LockoutIndex* index);

    /// Brief description of what process() does and when to call it.
    void process(uint32_t nowMs);

private:
    SettingsManager* settings_ = nullptr;
    LockoutIndex*    index_    = nullptr;
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
#include "lockout_index.h"

void MyModule::begin(SettingsManager* settings, LockoutIndex* index) {
    settings_ = settings;
    index_    = index;
}

void MyModule::process(uint32_t nowMs) {
    // Guard is optional but recommended for debug builds
    // settings_ and index_ are guaranteed non-null by contract
}
```

---

## Naming Conventions

| Thing | Convention | Example |
|---|---|---|
| Module class | `PascalCase` + `Module` suffix | `LockoutOrchestrationModule` |
| Service (stateless namespace) | `PascalCase` + `Service` suffix | `WifiSettingsApiService` |
| Private member | trailing underscore | `settings_`, `index_` |
| `begin()` parameter | no prefix/suffix | `settings`, `index` |
| Result struct | `<Module>Result` | `LockoutEnforcerResult` |
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
