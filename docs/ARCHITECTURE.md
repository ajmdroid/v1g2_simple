# v1g2_simple Architecture Plan

## Problem Statement
Current codebase has **poor change locality**:
- Adding a settings field requires changes in 5+ places across 3 files
- 832-line main loop makes changes risky
- Related code scattered hundreds of lines apart
- Impossible to keep all context in working memory

## Design Principles

### 1. Feature-Based Organization
Organize by **what the code does for users**, not technical layers.

### 2. Single Responsibility Modules
Each module owns ONE logical subsystem completely. If you need to change BLE behavior, you only edit BLE files.

### 3. Narrow Communication Interfaces
Modules talk through small, well-defined APIs. Changes inside a module don't leak out.

### 4. Testable Boundaries
Each module can be tested independently with mocks.

## Historical Context

An earlier full subsystem-rewrite proposal existed here; it is now historical and
not the active architecture. The implementation moved to incremental module
migration under `src/modules/` to reduce risk while preserving behavior.

Use the structure and responsibilities in the "Current Approach: Incremental
Module Migration" section below as the source of truth.

## Migration Strategy (Updated January 2026)

We're taking a more incremental approach than originally planned - extracting state and functions piece by piece rather than full subsystem rewrites. This is safer for a working production system.

### Current Approach: Incremental Module Migration

**Structure (February 25, 2026):**
```
src/
├── main.cpp                         (~1550 lines - orchestration + module wiring)
├── main_boot.cpp                    Boot-time helpers (reset reason, NVS health, panic breadcrumbs)
├── main_loop_phases.cpp             Loop phase router functions extracted from loop()
├── main_persist.cpp                 Periodic lockout/learner save state machines
├── modules/
│   ├── alert_persistence/           Alert on-screen persistence + state resets
│   │   └── alert_persistence_module.h/cpp
│   ├── auto_push/                   V1 profile auto-push on connect
│   │   └── auto_push_module.h/cpp
│   ├── ble/                         BLE data queue + connection state + runtime + cadence
│   │   ├── ble_queue_module.h/cpp
│   │   ├── connection_runtime_module.h/cpp
│   │   ├── connection_state_module.h/cpp
│   │   ├── connection_state_cadence_module.h/cpp
│   │   └── connection_state_dispatch_module.h/cpp
│   ├── camera/                      Camera runtime + index + background DB loading + API
│   │   ├── camera_api_service.h/cpp
│   │   ├── camera_runtime_module.h/cpp
│   │   ├── camera_index.h/cpp
│   │   ├── camera_data_loader.h/cpp
│   │   └── camera_event_log.h/cpp
│   ├── debug/                       Debug/metrics API + perf file endpoints
│   │   ├── debug_api_service.h/cpp
│   │   └── debug_perf_files_service.h/cpp
│   ├── display/                     Display pipeline + preview + restore + orchestration
│   │   ├── display_orchestration_module.h/cpp
│   │   ├── display_pipeline_module.h/cpp
│   │   ├── display_preview_module.h/cpp
│   │   └── display_restore_module.h/cpp
│   ├── gps/                         GPS runtime + observations + lockout safety + API
│   │   ├── gps_api_service.h/cpp
│   │   ├── gps_runtime_module.h/cpp
│   │   ├── gps_observation_log.h/cpp
│   │   └── gps_lockout_safety.h/cpp
│   ├── lockout/                     Lockout index/store/enforcer/learner runtime stack
│   │   ├── lockout_api_service.h/cpp
│   │   ├── lockout_orchestration_module.h/cpp
│   │   ├── lockout_pre_quiet_controller.h/cpp
│   │   ├── lockout_index.h/cpp
│   │   ├── lockout_store.h/cpp
│   │   ├── lockout_enforcer.h/cpp
│   │   ├── lockout_learner.h/cpp
│   │   ├── lockout_entry.h
│   │   ├── lockout_band_policy.h/cpp
│   │   ├── lockout_runtime_mute_controller.h/cpp
│   │   ├── signal_capture_module.h/cpp
│   │   ├── signal_observation_log.h/cpp
│   │   └── signal_observation_sd_logger.h/cpp
│   ├── obd/                         OBD API + state policy + runtime state machine
│   │   ├── obd_api_service.h/cpp
│   │   ├── obd_runtime_module.h/cpp
│   │   └── obd_state_policy.h
│   ├── perf/                        Debug macros
│   │   └── debug_macros.h
│   ├── power/                       Battery/power management
│   │   └── power_module.h/cpp
│   ├── speed/                       Speed source selection policy
│   │   └── speed_source_selector.h/cpp
│   ├── speed_volume/                Highway speed volume boost + speaker sync
│   │   ├── speed_volume_module.h/cpp
│   │   ├── speed_volume_runtime_module.h/cpp
│   │   └── speaker_quiet_sync_module.h/cpp
│   ├── system/                      Loop orchestration modules + event bus + maintenance
│   │   ├── system_event_bus.h
│   │   ├── parsed_frame_event_module.h/cpp
│   │   ├── periodic_maintenance_module.h/cpp
│   │   ├── loop_connection_early_module.h/cpp
│   │   ├── loop_power_touch_module.h/cpp
│   │   ├── loop_pre_ingest_module.h/cpp
│   │   ├── loop_settings_prep_module.h/cpp
│   │   ├── loop_ingest_module.h/cpp
│   │   ├── loop_display_module.h/cpp
│   │   ├── loop_post_display_module.h/cpp
│   │   ├── loop_runtime_snapshot_module.h/cpp
│   │   ├── loop_tail_module.h/cpp
│   │   └── loop_telemetry_module.h/cpp
│   ├── touch/                       Touch UI + tap gestures
│   │   ├── touch_ui_module.h/cpp
│   │   └── tap_gesture_module.h/cpp
│   ├── voice/                       Voice alert decisions + speed sync
│   │   ├── voice_module.h/cpp
│   │   └── voice_speed_sync_module.h/cpp
│   ├── volume_fade/                 Alert volume fade/restore
│   │   └── volume_fade_module.h/cpp
│   └── wifi/                        WiFi orchestration + boot policy + runtime + API services
│       ├── wifi_boot_policy.h
│       ├── wifi_api_response.h
│       ├── wifi_orchestrator_module.h/cpp
│       ├── wifi_auto_start_module.h/cpp
│       ├── wifi_priority_policy_module.h/cpp
│       ├── wifi_process_cadence_module.h/cpp
│       ├── wifi_runtime_module.h/cpp
│       ├── wifi_visual_sync_module.h/cpp
│       ├── backup_api_service.h/cpp
│       ├── wifi_autopush_api_service.h/cpp
│       ├── wifi_client_api_service.h/cpp
│       ├── wifi_control_api_service.h/cpp
│       ├── wifi_display_colors_api_service.h/cpp
│       ├── wifi_portal_api_service.h/cpp
│       ├── wifi_settings_api_service.h/cpp
│       ├── wifi_status_api_service.h/cpp
│       ├── wifi_time_api_service.h/cpp
│       ├── wifi_v1_devices_api_service.h/cpp
│       └── wifi_v1_profile_api_service.h/cpp
└── [core services: ble_client, display, settings, packet_parser, etc.]
    (see src/*.cpp for per-class implementation files)
```

**Module Responsibilities:**

| Module | Responsibility |
|--------|----------------|
| **AlertPersistenceModule** | Keeps alerts on-screen after they clear; provides state resets |
| **AutoPushModule** | Pushes V1 profiles on connect |
| **BleQueueModule** | Thread-safe BLE data queuing between callback and main loop |
| **ConnectionStateModule** | Manages BLE connect/disconnect display states |
| **ConnectionStateCadenceModule** | Throttles connection-state display transitions |
| **ConnectionStateDispatchModule** | Dispatches connection state processing at correct cadence |
| **ConnectionRuntimeModule** | BLE runtime state (connected, backpressure, last-rx) |
| **CameraRuntimeModule + CameraDataLoader** | Camera matching lifecycle + background database loading/swap |
| **CameraApiService** | Camera alert REST API endpoints |
| **DebugApiService + DebugPerfFilesService** | Debug/metrics REST API + perf file listing/download |
| **DisplayPipelineModule** | Owns alert rendering, display state updates, and V1 packet processing |
| **DisplayOrchestrationModule** | Coordinates parsed-frame rendering, lightweight refresh, and early-loop display |
| **DisplayPreviewModule** | Color/camera preview overlay lifecycle |
| **DisplayRestoreModule** | Restores display state after preview/settings overlay ends |
| **GpsRuntimeModule** | GPS ingest and fix/course/speed runtime state |
| **GpsApiService** | GPS lockout REST API endpoints |
| **Lockout stack** | Capture/observe/store/enforce/learn lockout state with best-effort persistence |
| **LockoutApiService + LockoutOrchestrationModule** | Lockout REST API + zone CRUD + pre-quiet controller |
| **ObdApiService + ObdStatePolicy** | OBD API contract and reconnect/backoff policy logic |
| **ObdRuntimeModule** | OBD auto-connect deferral and runtime process cadence |
| **PowerModule** | Battery monitoring, power button, sleep |
| **SpeedSourceSelector** | Runtime speed source arbitration (OBD-only policy) |
| **SpeedVolumeModule** | Boosts volume at highway speeds; defers to fade when fade owns volume |
| **SpeedVolumeRuntimeModule** | Orchestrates speed-volume + speaker-quiet sync per loop |
| **SpeakerQuietSyncModule** | Applies quiet-volume changes to hardware speaker amp |
| **SystemEventBus** | Bounded loop-local event channel for cross-module coordination |
| **ParsedFrameEventModule** | Collects parsed-frame signal from BLE queue for display orchestration |
| **PeriodicMaintenanceModule** | Rate-limited perf reporting, time saves, lockout learner ticks, persistence |
| **Loop phase modules** | `loop_connection_early`, `loop_power_touch`, `loop_pre_ingest`, `loop_settings_prep`, `loop_ingest`, `loop_display`, `loop_post_display`, `loop_runtime_snapshot`, `loop_tail`, `loop_telemetry` — each owns one phase of the main loop |
| **TouchUiModule** | Touch-based settings UI overlay |
| **TapGestureModule** | Triple-tap mute and other gestures |
| **VoiceModule** | All voice announcement decisions (priority/secondary/escalation) and cooldowns |
| **VoiceSpeedSyncModule** | Feeds speed samples from SpeedSourceSelector to VoiceModule |
| **VolumeFadeModule** | Decides when to fade/restore volume for long-running alerts |
| **WifiOrchestrator** | WiFi/web server lifecycle |
| **WifiAutoStartModule** | Deferred WiFi auto-start with V1 settle gate |
| **WifiPriorityPolicyModule** | WiFi/BLE priority balancing at runtime |
| **WifiProcessCadenceModule** | Throttles WiFi processing cadence |
| **WifiRuntimeModule** | Orchestrates WiFi auto-start, processing, and visual sync per loop |
| **WifiVisualSyncModule** | WiFi indicator redraw debounce |
| **WiFi API services** | Settings, status, colors, profiles, devices, backup, time, autopush, control, portal, client REST endpoints |

### Migration Process

Each step follows a strict protocol:
1. Add method/state to the target module
2. Update call sites in main.cpp
3. Remove old code from main.cpp
4. Build and test on hardware
5. Commit only after verification

Migration history is tracked via module commits and [CHANGELOG.md](../CHANGELOG.md).

## Success Metrics

### Before (January 27, 2026):
- Main loop: ~832 lines, main.cpp: ~3100 lines total
- Alert state scattered across 15+ static variables
- Change risk: HIGH (adjacent code interactions)

### After (February 25, 2026):
- main.cpp: ~1550 lines (orchestration + module wiring)
- main_boot.cpp: ~250 lines, main_loop_phases.cpp: ~210 lines, main_persist.cpp: ~445 lines
- 18 module directories, 156 module files in src/modules/
- 5 additional extracted core-service files (ble_runtime, obd_runtime, packet_parser_alerts, settings_restore, main_loop_phases)
- 85 native test suites, 934 test cases
- 7 CI contract scripts with 10 golden-file snapshots
- State consolidated in owning modules
- Change risk: LOW (isolated modules)

## Key Design Rules

1. **Modules receive dependencies via begin()** - dependency injection for testability
2. **Data flows DOWN** - main loop gets data and passes it to modules
3. **State lives in ONE place** - e.g., voice announcement state only in VoiceModule
4. **Incremental migration** - never break working functionality

## Dependency Injection Patterns

The codebase uses four patterns for dependency injection. Choose based on the module's coupling needs.

### Pattern 1: Direct Pointer Injection (Preferred for data dependencies)

Used by core modules. Dependencies are passed as pointers in `begin()`:

```cpp
class DisplayPipelineModule {
public:
    void begin(V1Display* display, PacketParser* parser, SettingsManager* settings, ...);
private:
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    // ...
};
```

**Use when:**
- Module needs to read state from dependencies (e.g., `parser->hasAlerts()`)
- Module calls methods on dependencies (e.g., `ble->setMute(true)`)
- Dependencies are core services used across many modules

**Examples:** `DisplayPipelineModule`, `VoiceModule`, `TapGestureModule`, `ConnectionStateModule`, `DisplayOrchestrationModule`

### Pattern 2: Callback Injection (Hybrid — pointers + `std::function<>`)

Used when a module needs direct access to some services but should trigger
cross-boundary actions without knowing the implementation:

```cpp
class TouchUiModule {
public:
    struct Callbacks {
        std::function<bool()> isWifiSetupActive;
        std::function<void()> stopWifiSetup;
        std::function<void()> startWifi;
        std::function<void()> restoreDisplay;
    };
    
    void begin(V1Display* disp, TouchHandler* touch, SettingsManager* settings, const Callbacks& cbs);
};
```

**Use when:**
- Actions cross architectural boundaries (e.g., touch module controlling WiFi)
- Module also needs direct pointer access for its primary data path

**Example:** `TouchUiModule` — stores three direct pointers for display/touch/settings, plus callbacks for WiFi lifecycle it shouldn't own

### Pattern 3: Providers (C function-pointers + `void*` context)

The dominant pattern for loop-phase modules and cross-cutting orchestrators. A
`struct Providers` carries C-style function pointers, each paired with an opaque
`void*` context. Avoids the overhead of `std::function<>` while giving full
type-erasure:

```cpp
class LoopConnectionEarlyModule {
public:
    struct Providers {
        ConnectionRuntimeSnapshot (*runConnectionRuntime)(
            void* ctx, uint32_t nowMs, ...) = nullptr;
        void* connectionRuntimeContext = nullptr;

        void (*showInitialScanning)(void* ctx) = nullptr;
        void* scanningContext = nullptr;
        // ...
    };

    void begin(const Providers& hooks);
    LoopConnectionEarlyResult process(const LoopConnectionEarlyContext& ctx);
};
```

Modules using this pattern typically also define paired value structs for
call-time data flow (`...Context` for inputs, `...Result` for outputs).

**Use when:**
- Module orchestrates multiple subsystems through narrow function interfaces
- `std::function<>` heap overhead is undesirable (hot loop path)
- Dependencies are best expressed as actions, not object pointers

**Examples:** All `Loop*Module` types (`LoopIngestModule`, `LoopConnectionEarlyModule`, …), `WifiRuntimeModule`, `ConnectionRuntimeModule`, `ConnectionStateDispatchModule`, `SpeedVolumeRuntimeModule`, `PeriodicMaintenanceModule`, `VoiceSpeedSyncModule`

### Pattern 4: Inline Pass-by-Reference (Stateless orchestrators)

No `begin()` at all. All dependencies are passed to `process()` on every call.
Used for thin orchestrator modules that hold minimal state:

```cpp
class ObdRuntimeModule {
public:
    void process(unsigned long nowMs,
                 bool obdServiceEnabled,
                 bool& obdAutoConnectPending,
                 unsigned long obdAutoConnectAtMs,
                 OBDHandler& obdHandler,
                 SpeedSourceSelector& speedSourceSelector);
};
```

**Use when:**
- Module is a thin state-gate with no persistent dependency graph
- All inputs are naturally available at the call site each loop iteration

**Example:** `ObdRuntimeModule`

### Choosing a Pattern

| Scenario | Pattern | Reason |
|----------|---------|--------|
| Read settings, display state | Direct pointers | Need full interface |
| Send BLE commands | Direct pointers | Calling methods |
| Toggle WiFi from touch | Callbacks | Crosses boundaries |
| Loop-phase orchestration | Providers | Narrow fn interface, no heap |
| Thin gating with no setup | Pass-by-ref | Stateless, all deps at call site |

### Testing Implications

**Direct pointers:** Create mock classes in `test/mocks/` that track method calls:
```cpp
class V1BLEClient {
public:
    int setMuteCalls = 0;
    bool setMute(bool mute) { setMuteCalls++; return true; }
};
```

**Callbacks:** Pass lambdas that set test flags:
```cpp
TouchUiModule::Callbacks cbs{
    .isWifiSetupActive = [&]() { return testWifiActive; },
    .startWifi = [&]() { startWifiCalled = true; }
};
```

**Providers:** Wire plain functions or static lambdas with context capture:
```cpp
LoopConnectionEarlyModule::Providers hooks{
    .runConnectionRuntime = [](void* ctx, uint32_t nowMs, ...) {
        return static_cast<TestFixture*>(ctx)->runConnectionRuntime(nowMs, ...);
    },
    .connectionRuntimeContext = &fixture,
};
```

**Pass-by-ref:** Call `process()` directly with test-local objects:
```cpp
ObdRuntimeModule mod;
mod.process(1000, true, pending, 0, mockObd, mockSelector);
```

## What This Enables

✅ **Change Locality**: Voice logic changes → edit VoiceModule; alert persistence → edit AlertPersistenceModule  
✅ **Testability**: Modules can be unit tested with mocked dependencies  
✅ **Clarity**: Related state grouped together, easier to understand  
✅ **Confidence**: Clear boundaries → certain only X changes  

## What We're NOT Doing

❌ **Not** making hundreds of tiny files (too scattered)  
❌ **Not** abstracting everything (premature generalization)  
❌ **Not** rewriting from scratch (incremental migration)  
❌ **Not** changing behavior (pure refactoring)
❌ **Not** rushing - test after every change
