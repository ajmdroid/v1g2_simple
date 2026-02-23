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

**Structure (February 2026):**
```
src/
├── main.cpp                         (~1164 lines - orchestration + integration control flow)
├── modules/
│   ├── alert_persistence/           Alert on-screen persistence + state resets
│   │   ├── alert_persistence_module.h
│   │   └── alert_persistence_module.cpp
│   ├── auto_push/                   V1 profile auto-push on connect
│   │   ├── auto_push_module.h
│   │   └── auto_push_module.cpp
│   ├── ble/                         BLE data queue + connection state + runtime
│   │   ├── ble_queue_module.h/cpp
│   │   ├── connection_runtime_module.h/cpp
│   │   └── connection_state_module.h/cpp
│   ├── camera/                      Camera runtime + index + background DB loading + API
│   │   ├── camera_api_service.h/cpp
│   │   ├── camera_runtime_module.h/cpp
│   │   ├── camera_index.h/cpp
│   │   ├── camera_data_loader.h/cpp
│   │   └── camera_event_log.h/cpp
│   ├── debug/                       Debug/metrics API
│   │   └── debug_api_service.h/cpp
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
│   ├── obd/                         OBD API + state policy
│   │   ├── obd_api_service.h/cpp
│   │   └── obd_state_policy.h
│   ├── perf/                        Debug macros
│   │   └── debug_macros.h
│   ├── power/                       Battery/power management
│   │   ├── power_module.h
│   │   └── power_module.cpp
│   ├── speed/                       OBD-only speed source selection policy
│   │   └── speed_source_selector.h/cpp
│   ├── speed_volume/                Highway speed volume boost
│   │   ├── speed_volume_module.h
│   │   └── speed_volume_module.cpp
│   ├── system/                      Event bus for loop-local module coordination
│   │   └── system_event_bus.h
│   ├── touch/                       Touch UI + tap gestures
│   │   ├── touch_ui_module.h/cpp
│   │   └── tap_gesture_module.h/cpp
│   ├── voice/                       Voice alert decisions
│   │   ├── voice_module.h
│   │   └── voice_module.cpp
│   ├── volume_fade/                 Alert volume fade/restore
│   │   ├── volume_fade_module.h
│   │   └── volume_fade_module.cpp
│   └── wifi/                        WiFi orchestration + boot policy + API services
│       ├── wifi_boot_policy.h
│       ├── wifi_orchestrator_module.h/cpp
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
└── [core services: ble_client, display, settings, etc.]
```

**Module Responsibilities:**

| Module | Responsibility |
|--------|----------------|
| **VoiceModule** | All voice announcement decisions (priority/secondary/escalation) and cooldowns |
| **AlertPersistenceModule** | Keeps alerts on-screen after they clear; provides state resets |
| **VolumeFadeModule** | Decides when to fade/restore volume for long-running alerts |
| **SpeedVolumeModule** | Boosts volume at highway speeds; defers to fade when fade owns volume |
| **DisplayPipelineModule** | Owns alert rendering, display state updates, and V1 packet processing |
| **BleQueueModule** | Thread-safe BLE data queuing between callback and main loop |
| **ConnectionStateModule** | Manages BLE connect/disconnect display states |
| **TouchUiModule** | Touch-based settings UI overlay |
| **TapGestureModule** | Triple-tap mute and other gestures |
| **PowerModule** | Battery monitoring, power button, sleep |
| **AutoPushModule** | Pushes V1 profiles on connect |
| **CameraRuntimeModule + CameraDataLoader** | Camera matching lifecycle + background database loading/swap |
| **CameraApiService** | Camera alert REST API endpoints |
| **Lockout stack** | Capture/observe/store/enforce/learn lockout state with best-effort persistence |
| **LockoutApiService + LockoutOrchestrationModule** | Lockout REST API + zone CRUD + pre-quiet controller |
| **GpsRuntimeModule** | GPS ingest and fix/course/speed runtime state |
| **GpsApiService** | GPS lockout REST API endpoints |
| **SpeedSourceSelector** | Runtime speed source arbitration (OBD-only policy) |
| **ObdApiService + ObdStatePolicy** | OBD API contract and reconnect/backoff policy logic |
| **DebugApiService** | Debug/metrics REST API endpoints |
| **SystemEventBus** | Bounded loop-local event channel for cross-module coordination |
| **WifiOrchestrator** | WiFi/web server lifecycle |
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

### After (February 2026):
- main.cpp: ~1164 lines (orchestration + integration control flow)
- 18 module directories in src/modules/
- State consolidated in owning modules
- Change risk: LOW (isolated modules)

## Key Design Rules

1. **Modules receive dependencies via begin()** - dependency injection for testability
2. **Data flows DOWN** - main loop gets data and passes it to modules
3. **State lives in ONE place** - e.g., voice announcement state only in VoiceModule
4. **Incremental migration** - never break working functionality

## Dependency Injection Patterns

The codebase uses two patterns for dependency injection. Both are valid; choose based on the module's needs.

### Pattern 1: Direct Pointer Injection (Preferred for data dependencies)

Used by most modules. Dependencies are passed as pointers in `begin()`:

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

**Examples:** `DisplayPipelineModule`, `VoiceModule`, `TapGestureModule`, `ConnectionStateModule`

### Pattern 2: Callback Injection (For action isolation)

Used when a module should trigger actions without knowing the implementation:

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
- Module should not depend on specific service implementations
- Actions cross architectural boundaries (e.g., touch module controlling WiFi)
- You want to decouple for easier testing or future flexibility

**Example:** `TouchUiModule` - handles BOOT button but shouldn't know about `WifiManager` directly

### Choosing a Pattern

| Scenario | Pattern | Reason |
|----------|---------|--------|
| Read settings, display state | Direct pointers | Need full interface |
| Send BLE commands | Direct pointers | Calling methods |
| Toggle WiFi from touch | Callbacks | Crosses boundaries |
| Trigger display refresh | Callbacks | Implementation-agnostic |

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
