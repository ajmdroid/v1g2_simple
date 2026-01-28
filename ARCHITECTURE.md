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
│       └── camera_manager.h/cpp        (existing)
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

## Migration Strategy

### Phase 1: Extract BLE (Week 1)
- Create `subsystems/ble_subsystem/`
- Move 200 lines from main loop → `BLESubsystem::update()`
- Main loop calls `ble.update()` and `ble.getLatestAlert()`
- **Test thoroughly, commit**

### Phase 2: Extract GPS/Camera (Week 2)  
- Create `subsystems/gps_subsystem/`
- Move GPS state machine + camera query logic
- **Test, commit**

### Phase 3: Extract Display Rendering (Week 3)
- Create `subsystems/display_subsystem/`
- Move alert rendering logic
- **Test, commit**

### Phase 4: Unify Settings API (Week 4)
- Create `api/settings_api.cpp`
- Move ALL settings endpoints from wifi_manager
- **Test, commit**

## Success Metrics

### Before:
- Adding log flag: touch 5 places across 3 files
- Main loop: 832 lines
- Change risk: HIGH (adjacent code interactions)

### After:
- Adding log flag: touch 1 file (settings_api.cpp)
- Main loop: ~200 lines (orchestration)
- Change risk: LOW (isolated modules)
- Test coverage: Each module independently testable

## Key Design Rules

1. **Subsystems never directly call each other** - only through main orchestration
2. **Data flows DOWN** - main loop gets data and passes it
3. **State lives in ONE place** - GPS position only in GPSSubsystem
4. **API layer is isolated** - web handlers never directly modify hardware

## File Size Targets

- `main.cpp`: 150-250 lines (orchestration)
- Each subsystem: 200-400 lines (single responsibility)
- Each API file: 200-300 lines (one domain)
- Drivers: Keep as-is (already focused)

## What This Enables

✅ **Change Locality**: Modify BLE → edit BLE files only  
✅ **Testability**: Mock GPSSubsystem, test alert logic  
✅ **Onboarding**: New dev reads 300 lines, not 3000  
✅ **Parallel Work**: Two features, two modules, no conflicts  
✅ **Confidence**: Clear boundaries → certain only X changes  

## What We're NOT Doing

❌ **Not** making hundreds of tiny files (too scattered)  
❌ **Not** abstracting everything (premature generalization)  
❌ **Not** rewriting from scratch (incremental migration)  
❌ **Not** changing behavior (pure refactoring)  

## Next Steps

1. Review this architecture → adjust if needed
2. Create subsystems/ directory structure  
3. Start Phase 1: BLESubsystem extraction
4. Test on hardware after each phase
5. Commit working code frequently
