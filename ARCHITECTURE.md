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

## Migration Strategy (Updated January 2026)

We're taking a more incremental approach than originally planned - extracting state and functions piece by piece rather than full subsystem rewrites. This is safer for a working production system.

### Current Approach: Incremental Module Migration

**Structure:**
```
src/
├── main.cpp                         (~2800 lines, shrinking incrementally)
├── modules/
│   ├── alert_persistence/
│   │   ├── alert_persistence_module.h   (~110 lines)
│   │   └── alert_persistence_module.cpp (~140 lines)
│   ├── voice/
│   │   ├── voice_module.h               (~220 lines)
│   │   └── voice_module.cpp             (~450 lines)
│   ├── volume_fade/
│   │   ├── volume_fade_module.h         (~90 lines)
│   │   └── volume_fade_module.cpp       (~130 lines)
│   └── speed_volume/
│       ├── speed_volume_module.h        (~70 lines)
│       └── speed_volume_module.cpp      (~110 lines)
└── [existing files unchanged]
```

**VoiceModule** owns all voice announcement decisions (priority/secondary/escalation) and cooldowns. main.cpp only builds a `VoiceContext` and executes the returned `VoiceAction`.

**AlertPersistenceModule** keeps alerts on-screen briefly after they clear and provides one-call state resets for alert-related modules.

**VolumeFadeModule** decides when to fade/restore volume for long-running alerts.

**SpeedVolumeModule** boosts/restores volume at highway speeds while respecting active fades.

### Migration Process

Each step follows a strict protocol:
1. Add method/state to the target module (Voice, Alert persistence, Volume fade)
2. Update call sites in main.cpp
3. Remove old code from main.cpp
4. Build and test on hardware
5. Commit only after verification

See [REFACTOR_LOG.md](REFACTOR_LOG.md) for detailed step-by-step progress.

### Future Phases (Tentative)

Next extractions (if we keep shrinking main.cpp):
- **Display Module**: Color preview, demo mode
- **Settings API**: Consolidate web endpoints
- **GPS Module**: Speed caching, lockout queries

Or we may keep the current structure if it's working well.

## Success Metrics

### Before (January 27, 2026):
- Main loop: ~832 lines, main.cpp: ~3100 lines total
- Alert state scattered across 15+ static variables
- Change risk: HIGH (adjacent code interactions)

### Current Progress:
- VoiceModule: ~430 lines (all voice decisions consolidated)
- AlertPersistenceModule: ~120 lines (alert persistence + clears)
- VolumeFadeModule: ~120 lines (fade/restore decisions)
- SpeedVolumeModule: ~90 lines (boost/restore decisions; defers to fade when fade owns volume)
- main.cpp: ~2800 lines (reduced ~300 lines; voice/speed/fade logic removed)
- Alert and voice tracking consolidated in modules
- Change risk: LOWER (state/logic in modules)

### Target:
- main.cpp: ~2000 lines (orchestration + features not yet extracted)
- Each module: 200-600 lines (single responsibility)
- Change risk: LOW (isolated modules)

## Key Design Rules

1. **Modules receive dependencies via begin()** - dependency injection for testability
2. **Data flows DOWN** - main loop gets data and passes it to modules
3. **State lives in ONE place** - e.g., voice announcement state only in VoiceModule
4. **Incremental migration** - never break working functionality

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
