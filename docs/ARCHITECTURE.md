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

## Proposed Module Structure

```
src/
├── main.cpp                    (150-250 lines: orchestration only)
│
├── subsystems/
│   ├── ble_subsystem/
│   │   ├── ble_subsystem.h     → BLESubsystem class
│   │   ├── ble_subsystem.cpp   → Connection, V1 protocol, alert parsing
│   │   └── v1_protocol.cpp     → V1 packet handling (extracted from packet_parser)
│   │
│   ├── gps_subsystem/
│   │   ├── gps_subsystem.h     → GPSSubsystem class
│   │   ├── gps_subsystem.cpp   → GPS state, heading, speed smoothing
│   │   └── camera_query.cpp    → Camera distance/bearing calculations
│   │
│   ├── display_subsystem/
│   │   ├── display_subsystem.h → DisplaySubsystem class
│   │   ├── display_subsystem.cpp → Orchestrates rendering
│   │   ├── alert_renderer.cpp  → Alert display logic
│   │   └── status_renderer.cpp → Icons, battery, GPS status
│   │
│   └── audio_subsystem/
│       ├── audio_subsystem.h   → AudioSubsystem class
│       └── audio_subsystem.cpp → Alert audio, muting, volume
│
├── features/                   (Higher-level features using subsystems)
│   ├── lockout/
│   │   ├── lockout_manager.h/cpp       (existing)
│   │   └── auto_lockout_manager.h/cpp  (existing)
│   │
│   └── camera_alerts/
│       ├── camera_runtime_module.h/cpp (existing)
│       ├── camera_index.h/cpp          (existing)
│       ├── camera_data_loader.h/cpp    (existing)
│       └── camera_event_log.h/cpp      (existing)
│
├── api/                        (Web API - clear boundary)
│   ├── settings_api.h/cpp      → ALL settings endpoints + serialization
│   ├── gps_api.h/cpp           → GPS/lockout data endpoints
│   └── system_api.h/cpp        → Device info, logs, backup/restore
│
├── core/                       (Shared utilities)
│   ├── settings.h/cpp          (existing - just storage)
│   ├── storage_manager.h/cpp   (existing)
│   ├── debug_logger.h/cpp      (existing)
│   └── perf_metrics.h/cpp      (existing)
│
└── drivers/                    (Hardware interfaces)
    ├── display.h/cpp           (existing - low level only)
    ├── gps_handler.h/cpp       (existing - UART/parsing only)
    ├── audio_beep.h/cpp        (existing)
    └── touch_handler.h/cpp     (existing)
```

## Module Responsibilities

### BLESubsystem
**Owns:** V1 connection, ESP32 proxy, alert parsing, packet processing  
**Interface:**
```cpp
void update();                          // Called each loop
bool isConnected();
bool hasNewAlert();
AlertData getLatestAlert();
V1Settings getV1Settings();
```
**Touches:** Only BLE, packet parsing code. Never directly touches display/audio.

### GPSSubsystem  
**Owns:** GPS state, position, speed, heading, camera database queries  
**Interface:**
```cpp
void update();
bool isReady();                        // 15s sustained fix
float getSpeed();
float getHeading();
GPSPosition getPosition();
std::vector<Camera> queryCameras(float radius);
```
**Touches:** Only GPS, camera DB. Main loop asks for data, doesn't pull it.

### DisplaySubsystem
**Owns:** All rendering decisions, dimming, demo mode  
**Interface:**
```cpp
void update();
void showAlert(AlertData alert, float speed);
void showStatus();
void showDemo();
void setBrightness(uint8_t level);
```
**Touches:** Only display driver. Never directly reads BLE/GPS.

### AudioSubsystem
**Owns:** Alert audio, voice, muting, volume fade  
**Interface:**
```cpp
void update();
void playAlert(AlertData alert, float speed);
void playVoice(const char* text);
void setVolume(uint8_t level);
```
**Touches:** Only audio driver. Gets alert data passed in.

### SettingsAPI
**Owns:** ALL settings HTTP endpoints, JSON serialization  
**Interface:**
```cpp
void registerEndpoints(WebServer& server);
// Internally handles:
//   GET  /api/settings  → serializeAllSettings()
//   POST /api/displaycolors → handleColorsSave()
//   etc.
```
**Why unified:** Adding a setting means editing ONE file, not 5.

## Migration Strategy (Updated January 2026)

We're taking a more incremental approach than originally planned - extracting state and functions piece by piece rather than full subsystem rewrites. This is safer for a working production system.

### Current Approach: Incremental Module Migration

**Structure (February 2026):**
```
src/
├── main.cpp                         (~1290 lines - orchestration + integration control flow)
├── modules/
│   ├── alert_persistence/           Alert on-screen persistence + state resets
│   │   ├── alert_persistence_module.h
│   │   └── alert_persistence_module.cpp
│   ├── auto_push/                   V1 profile auto-push on connect
│   │   ├── auto_push_module.h
│   │   └── auto_push_module.cpp
│   ├── ble/                         BLE data queue + connection state
│   │   ├── ble_queue_module.h/cpp
│   │   └── connection_state_module.h/cpp
│   ├── camera/                      Camera runtime + index + background DB loading
│   │   ├── camera_runtime_module.h/cpp
│   │   ├── camera_index.h/cpp
│   │   ├── camera_data_loader.h/cpp
│   │   └── camera_event_log.h/cpp
│   ├── display/                     Display pipeline + preview + restore
│   │   ├── display_pipeline_module.h/cpp
│   │   ├── display_preview_module.h/cpp
│   │   └── display_restore_module.h/cpp
│   ├── gps/                         GPS runtime + observations + lockout safety
│   │   ├── gps_runtime_module.h/cpp
│   │   ├── gps_observation_log.h/cpp
│   │   └── gps_lockout_safety.h/cpp
│   ├── lockout/                     Lockout index/store/enforcer/learner runtime stack
│   │   ├── lockout_index.h/cpp
│   │   ├── lockout_store.h/cpp
│   │   ├── lockout_enforcer.h/cpp
│   │   ├── lockout_learner.h/cpp
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
│   │   ├── speed_source_selector.h/cpp
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
│   └── wifi/                        WiFi orchestration
│       ├── wifi_orchestrator_module.h
│       └── wifi_orchestrator_module.cpp
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
| **Lockout stack** | Capture/observe/store/enforce/learn lockout state with best-effort persistence |
| **GpsRuntimeModule** | GPS ingest and fix/course/speed runtime state |
| **SpeedSourceSelector** | Runtime speed source arbitration (OBD-only policy) |
| **ObdApiService + ObdStatePolicy** | OBD API contract and reconnect/backoff policy logic |
| **SystemEventBus** | Bounded loop-local event channel for cross-module coordination |
| **WifiOrchestrator** | WiFi/web server lifecycle |

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
- main.cpp: ~1290 lines (orchestration + integration control flow)
- 17 module directories in src/modules/
- State consolidated in owning modules
- Change risk: LOW (isolated modules)

Note: `_disabled/` remains reference-only historical code and is not part of runtime builds.

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
