# Camera Proximity Alerts — Implementation Plan v5

Phase 2 of road map integration. Phase 1 delivered `road_map.bin` v2
with 75,440 cameras in PSRAM (commits `01ebe60`, `636d9b6`, `0df823d`).

---

## 1. Requirements

| Req | Detail |
|-----|--------|
| **Display** | Distance (feet) in frequency area, same Segment7 font @ 75px. Type label below. Purple `0x780F`. Front arrow only. |
| **Precedence** | Live V1 > Persisted V1 > Camera > Resting. Camera never interrupts V1. |
| **Bearing filter** | Only show cameras within ±90° of travel direction ("ahead"). Conservative: unknown bearing or stale course → show alert. |
| **Voice** | Two-stage: far (1000 ft) and near (500 ft). Each independently disableable. Same volume path as V1 voice. |
| **Settings** | Master on/off, range, 4 type toggles, 2 colors, 2 voice stage toggles. NVS + SD backup/restore. |
| **Web UI** | `/cameras` page with toggles per type + range + voice settings. |
| **Metrics** | No impact — only `dirty.frequency` + `dirty.arrow` touched. |
| **Clearing** | Alert clears when camera exits range or behind ±90° window. |
| **Mode** | V1 connected mode only (standalone is future). |

---

## 2. Camera Type Enum

Flags in `road_map.bin` are discrete type IDs, not bitmasks.
Proven via exhaustive DB query: zero records outside `{1,2,3,4}`.

```cpp
// camera_alert_types.h
enum CameraType : uint8_t {
    CAM_SPEED     = 1,   // Speed camera (11 in dataset)
    CAM_RED_LIGHT = 2,   // Red light camera (1,013)
    CAM_BUS_LANE  = 3,   // Bus lane camera (88)
    CAM_ALPR      = 4    // License plate reader (74,328)
};

inline const char* cameraTypeLabel(CameraType t) {
    switch (t) {
        case CAM_SPEED:     return "SPEED CAM";
        case CAM_RED_LIGHT: return "RED LIGHT";
        case CAM_BUS_LANE:  return "BUS LANE";
        case CAM_ALPR:      return "ALPR";
        default:            return "CAMERA";
    }
}
```

---

## 3. Settings (10 new NVS fields)

Added to `V1Settings` struct in `settings.h`:

```cpp
// Camera alert settings
bool cameraAlertsEnabled;                // Master on/off (default: true)
uint16_t cameraAlertRangeFt;             // Max alert range in feet (default: 5280 = 1 mile)
bool cameraTypeSpeed;                    // Show speed cameras (default: true)
bool cameraTypeRedLight;                 // Show red light cameras (default: true)
bool cameraTypeBusLane;                  // Show bus lane cameras (default: true)
bool cameraTypeALPR;                     // Show ALPR cameras (default: true)
uint16_t colorCameraArrow;               // Arrow color for camera alerts (default: 0x780F purple)
uint16_t colorCameraText;                // Frequency/label color (default: 0x780F purple)
bool cameraVoiceFarEnabled;              // Voice at 1000 ft (default: true)
bool cameraVoiceNearEnabled;             // Voice at 500 ft (default: true)
```

**Defaults in constructor:**
```cpp
cameraAlertsEnabled(true),
cameraAlertRangeFt(5280),
cameraTypeSpeed(true),
cameraTypeRedLight(true),
cameraTypeBusLane(true),
cameraTypeALPR(true),
colorCameraArrow(0x780F),
colorCameraText(0x780F),
cameraVoiceFarEnabled(true),
cameraVoiceNearEnabled(true),
```

**NVS keys** (in `settings_nvs.cpp`):
| Key | Type | Field |
|-----|------|-------|
| `camEnabled` | Bool | `cameraAlertsEnabled` |
| `camRangeFt` | UShort | `cameraAlertRangeFt` |
| `camTypeSpd` | Bool | `cameraTypeSpeed` |
| `camTypeRed` | Bool | `cameraTypeRedLight` |
| `camTypeBus` | Bool | `cameraTypeBusLane` |
| `camTypeALPR` | Bool | `cameraTypeALPR` |
| `camColorArr` | UShort | `colorCameraArrow` |
| `camColorTxt` | UShort | `colorCameraText` |
| `camVoiceFar` | Bool | `cameraVoiceFarEnabled` |
| `camVoiceNear` | Bool | `cameraVoiceNearEnabled` |

**SD_BACKUP_VERSION**: `9` → `10`

All 10 fields added to `settings_backup.cpp` (JSON write) and
`settings_restore.cpp` (JSON read) with matching key names.

---

## 4. Audio Clips

Generated via `tools/generate_camera_audio.sh` using the canonical
macOS Samantha pipeline:

```bash
say -v Samantha -r 210 -o file.aiff "text"
ffmpeg -y -i file.aiff -ar 22050 -ac 1 -f s16le -acodec pcm_s16le file.raw
ffmpeg -y -f s16le -ar 22050 -ac 1 -i file.raw -f mulaw -ar 22050 file.mul
```

### Clip inventory (5 new files)

| File | Text | Purpose |
|------|------|---------|
| `cam_speed.mul` | "speed camera" | Speed camera type label |
| `cam_red_light.mul` | "red light camera" | Red light type label |
| `cam_bus_lane.mul` | "bus lane camera" | Bus lane type label |
| `cam_alpr.mul` | "license plate reader" | ALPR type label |
| `cam_close.mul` | "close" | Near-stage suffix |

**Canonical path**: `tools/freq_audio/mulaw/` (git-tracked).
Build copies to `data/audio/` (ephemeral, nuked by web deploy).

### Voice composition

| Stage | Clips played | Example |
|-------|-------------|---------|
| Far (1000 ft) | `cam_<type>.mul` + `dir_ahead.mul` | "speed camera ahead" |
| Near (500 ft) | `cam_<type>.mul` + `cam_close.mul` | "speed camera close" |

Reuses existing `dir_ahead.mul` for the far stage.

### Playback function

```cpp
// In audio_voice.cpp (same file — start_sd_audio_task is static)
void play_camera_voice(CameraType type, bool isNear) {
    SDAudioTaskParams params;
    params.numClips = 0;

    const char* typeFile = nullptr;
    switch (type) {
        case CAM_SPEED:     typeFile = "cam_speed.mul"; break;
        case CAM_RED_LIGHT: typeFile = "cam_red_light.mul"; break;
        case CAM_BUS_LANE:  typeFile = "cam_bus_lane.mul"; break;
        case CAM_ALPR:      typeFile = "cam_alpr.mul"; break;
        default: return;
    }

    snprintf(params.filePaths[params.numClips++], 48,
             "%s/%s", AUDIO_PATH, typeFile);
    snprintf(params.filePaths[params.numClips++], 48,
             "%s/%s", AUDIO_PATH, isNear ? "cam_close.mul" : "dir_ahead.mul");

    start_sd_audio_task(params);
}
```

---

## 5. Camera Alert Module

### New files
- `src/modules/camera/camera_alert_module.h`
- `src/modules/camera/camera_alert_module.cpp`
- `include/camera_alert_types.h`

### Constants

```cpp
static constexpr uint16_t CAMERA_FAR_THRESHOLD_CM  = 30480;  // 1000 ft
static constexpr uint16_t CAMERA_NEAR_THRESHOLD_CM = 15240;  // 500 ft
static constexpr uint32_t ENCOUNTER_EXPIRE_MS      = 10000;  // 10s no-see → reset
static constexpr uint32_t CAMERA_COURSE_MAX_AGE_MS = 3000;   // Match GPS SAMPLE_MAX_AGE_MS
static constexpr float    CAMERA_AHEAD_HALF_ANGLE   = 90.0f; // ±90° ahead cone
```

### Camera encounter identity & voice anti-repeat

Camera identity is `(latE5, lonE5)` of the CameraResult — fixed PSRAM
coordinates that never jitter (unlike GPS position).

```cpp
struct CameraEncounter {
    int32_t  camLatE5 = 0;
    int32_t  camLonE5 = 0;
    bool     farAnnounced  = false;   // One-shot: 1000 ft voice fired
    bool     nearAnnounced = false;   // One-shot: 500 ft voice fired
    uint32_t lastSeenMs = 0;          // For expiry detection
};
```

**Rules:**
- **Same camera**: `camLatE5 == result.latE5 && camLonE5 == result.lonE5`
- **New camera**: Different coords → full reset, clear both voice flags
- **Encounter expiry**: `nearestCamera()` returns `valid==false` for
  `> ENCOUNTER_EXPIRE_MS` (10s) → clear encounter. Re-approach same
  physical camera after expiry = fresh encounter, voice fires again.
- **GPS jitter immunity**: `distanceCm` may oscillate ±5m but one-shot
  flags prevent re-triggering. Camera identity comes from PSRAM, not GPS.
- **No global cooldown needed**: Unlike V1 voice (5s cooldown), the
  one-shot encounter flags provide complete protection.

### Bearing relevance check

```cpp
bool CameraAlertModule::isCameraAhead(const CameraResult& cam,
                                       const GpsRuntimeStatus& gps) const {
    // Unknown bearing → show (conservative)
    if (cam.bearing == 0xFFFF) return true;

    // Course stale or invalid → show (conservative)
    if (!gps.courseValid || gps.courseAgeMs > CAMERA_COURSE_MAX_AGE_MS) return true;

    // Angular difference with wraparound
    float diff = fabsf(static_cast<float>(cam.bearing) - gps.courseDeg);
    if (diff > 180.0f) diff = 360.0f - diff;

    return diff <= CAMERA_AHEAD_HALF_ANGLE;
}
```

### Process method (called from main loop)

```cpp
struct CameraAlertState {
    bool     active = false;        // Camera alert being displayed
    CameraType type = CAM_ALPR;    // Type of active camera
    uint16_t distanceFt = 0;        // Distance in feet for display
};

CameraAlertState CameraAlertModule::process(uint32_t nowMs,
                                             bool v1AlertActive,
                                             bool v1PersistedActive) {
    CameraAlertState result;

    // Precedence: V1 owns the display when active
    if (v1AlertActive || v1PersistedActive) {
        // Don't expire encounter while V1 is active — camera may still
        // be nearby, just display-suppressed
        if (encounter_.lastSeenMs != 0) {
            encounter_.lastSeenMs = nowMs;
        }
        return result;  // active=false
    }

    const V1Settings& s = settings_->get();
    if (!s.cameraAlertsEnabled || !gps_) return result;

    // Get GPS snapshot
    GpsRuntimeStatus gpsStatus = gps_->snapshot(nowMs);
    if (!gpsStatus.hasFix) return result;

    // Query nearest camera
    CameraResult cam = roadMap_->nearestCamera(gpsStatus.latE5, gpsStatus.lonE5);

    if (!cam.valid) {
        // Check encounter expiry
        if (encounter_.lastSeenMs != 0 &&
            (nowMs - encounter_.lastSeenMs) > ENCOUNTER_EXPIRE_MS) {
            encounter_ = {};
        }
        return result;
    }

    // Type filtering
    CameraType type = static_cast<CameraType>(cam.flags);
    if (!isCameraTypeEnabled(s, type)) return result;

    // Range filtering (convert cm → ft)
    uint32_t distanceFt = static_cast<uint32_t>(cam.distanceCm) * 100 / 3048;
    if (distanceFt > s.cameraAlertRangeFt) return result;

    // Bearing filter
    if (!isCameraAhead(cam, gpsStatus)) return result;

    // --- Camera alert is active ---

    // Update encounter identity
    if (cam.latE5 != encounter_.camLatE5 || cam.lonE5 != encounter_.camLonE5) {
        encounter_ = {};
        encounter_.camLatE5 = cam.latE5;
        encounter_.camLonE5 = cam.lonE5;
    }
    encounter_.lastSeenMs = nowMs;

    // Voice decisions (one-shot per encounter)
    if (s.cameraVoiceNearEnabled && !encounter_.nearAnnounced &&
        cam.distanceCm <= CAMERA_NEAR_THRESHOLD_CM) {
        encounter_.nearAnnounced = true;
        play_camera_voice(type, true);   // "speed camera close"
    } else if (s.cameraVoiceFarEnabled && !encounter_.farAnnounced &&
               cam.distanceCm <= CAMERA_FAR_THRESHOLD_CM) {
        encounter_.farAnnounced = true;
        play_camera_voice(type, false);  // "speed camera ahead"
    }

    result.active = true;
    result.type = type;
    result.distanceFt = static_cast<uint16_t>(std::min(distanceFt, (uint32_t)9999));
    return result;
}
```

---

## 6. Display Integration

### What draws

Camera alert uses **only** `dirty.frequency` and `dirty.arrow`:
- **Frequency area**: Distance in feet (e.g. "2640"), Segment7 font @ 75px, purple `0x780F`
- **Below frequency**: Type label (e.g. "ALPR"), smaller font, purple `0x780F`
- **Arrow**: Front arrow only, purple `0x780F`
- No band indicators, no signal bars, no bogey counter changes

### Where camera display is triggered

In `display_pipeline_module.cpp`, the IDLE path (no V1 alerts).
After persistence check fails, instead of going straight to resting:

```
EXISTING:
  if persistSec > 0 && persisted.isValid:
    if shouldShowPersisted → display->updatePersisted(...)
    else → clearPersistence; display->update(state)  // resting ← CAMERA GOES HERE
  else:
    clearPersistence; display->update(state)          // resting ← AND HERE

NEW:
  if persistSec > 0 && persisted.isValid:
    if shouldShowPersisted → display->updatePersisted(...)
    else → clearPersistence;
           if (cameraState.active) display->updateCamera(...) else display->update(state)
  else:
    clearPersistence;
    if (cameraState.active) display->updateCamera(...) else display->update(state)
```

### New display method

```cpp
// In display.h
void updateCamera(uint16_t distanceFt, const char* typeLabel,
                  uint16_t arrowColor, uint16_t textColor,
                  const DisplayState& state);
```

Implementation draws:
1. Front arrow in `arrowColor`
2. Distance in Segment7 font in `textColor`
3. Type label in `textColor` below frequency area
4. All other elements from `state` (normal resting: WiFi, BLE, GPS badges)

### Lightweight refresh gate

In `display_orchestration_module.h`:
```cpp
struct DisplayOrchestrationRefreshContext {
    uint32_t nowMs = 0;
    bool bootSplashHoldActive = false;
    bool overloadLateThisLoop = false;
    bool pipelineRanThisLoop = false;
    bool cameraAlertActive = false;  // NEW
};
```

In `processLightweightRefresh()`, the else branch:
```cpp
// BEFORE:
} else {
    display->refreshFrequencyOnly(0, BAND_NONE, false, false);
}

// AFTER:
} else if (!ctx.cameraAlertActive) {
    display->refreshFrequencyOnly(0, BAND_NONE, false, false);
}
// Camera owns frequency area — skip lightweight clear
```

### Precedence summary

| State | Frequency area | Arrow | Lightweight refresh |
|-------|---------------|-------|-------------------|
| Live V1 alert | V1 freq/band | V1 direction | `refreshFrequencyOnly(freq, band, ...)` |
| Persisted V1 | Persisted freq | Persisted dir | `signalPriorityActive=true` path |
| Camera alert | Distance (ft) | Front, purple | `cameraAlertActive=true` → skip clear |
| Resting | Blank | None | `refreshFrequencyOnly(0, BAND_NONE, ...)` |

---

## 7. Web UI — `/cameras` Page

### Route: `interface/src/routes/cameras/+page.svelte`

### API endpoints (in `wifi_api_cameras.cpp`)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/cameras/settings` | Read all 10 camera settings |
| POST | `/api/cameras/settings` | Update camera settings |
| GET | `/api/cameras/status` | Live camera alert state (for dev page) |

### UI layout

```
┌────────────────────────────────────────┐
│ Camera Alerts                    [ON]  │  Master toggle
├────────────────────────────────────────┤
│ Detection Range         [====] 1.0 mi  │  Slider: 1000-10560 ft
├────────────────────────────────────────┤
│ Camera Types                           │
│   Speed Cameras                  [ON]  │
│   Red Light Cameras              [ON]  │
│   Bus Lane Cameras               [ON]  │
│   ALPR / Plate Readers           [ON]  │
├────────────────────────────────────────┤
│ Voice Alerts                           │
│   Announce at 1000 ft            [ON]  │
│   Announce at 500 ft             [ON]  │
├────────────────────────────────────────┤
│ Colors                                 │
│   Arrow Color          [■] 0x780F      │
│   Text Color           [■] 0x780F      │
└────────────────────────────────────────┘
```

DaisyUI toggle-primary for all booleans, range slider for distance.

### Navigation

Add "Cameras" link to sidebar/nav in `+layout.svelte`, icon: 📷 or
camera SVG, positioned after "Lockouts".

### Contract update

In `scripts/check_wifi_api_contract.py`:
```python
ROUTE_PREFIXES = (
    "/api/settings/backup",
    "/api/settings/restore",
    "/api/debug/",
    "/api/gps/",
    "/api/lockouts/",
    "/api/cameras/",      # NEW
)
POLICY_CALLBACK_PREFIXES = (
    "/api/gps/",
    "/api/lockouts/",
    "/api/cameras/",      # NEW
)
```

---

## 8. Main Loop Integration

Camera alert module runs inside `processLoopDisplayPreWifiPhase`,
specifically in `LoopDisplayModule::process()`, between the display
pipeline and lightweight refresh:

```
Call order in LoopDisplayModule::process():
1. collectParsedSignal()
2. runParsedFrame()                    → orchestration
3. runDisplayPipeline()                → displayPipelineModule.handleParsed()
4. ★ cameraAlertModule.process()      → NEW: query + voice + state
5. runLightweightRefresh()             → uses cameraAlertActive flag
```

The camera module receives `v1AlertActive` and `v1PersistedActive`
from step 3's output so it knows whether V1 owns the display.

The `CameraAlertState` result feeds into:
- Step 3 (next frame): `displayPipelineModule` checks `cameraState.active`
  in the IDLE path to decide between `updateCamera()` and `update(state)`
- Step 5: `ctx.cameraAlertActive` gates lightweight refresh

---

## 9. GPS Course Freshness

**Constant**: `CAMERA_COURSE_MAX_AGE_MS = 3000` (defined in camera
alert module, matches GPS module's `SAMPLE_MAX_AGE_MS`).

**Behavior when course goes stale mid-alert:**
- `isCameraAhead()` returns `true` (conservative)
- Camera alert continues displaying
- No direction-based dismissal when course is unknown
- Alert clears only when camera exits range or `nearestCamera()` returns
  invalid for 10s

---

## 10. Test Plan

### Suite 1: `test_camera_alert_module/test_camera_alert_module.cpp`

Framework: Unity (native). Mocks: PacketParser, SettingsManager,
RoadMapReader (stub `nearestCamera()`), GpsRuntimeModule (stub
`snapshot()`).

| # | Test | Validates |
|---|------|-----------|
| 1 | `test_no_camera_no_alert` | `valid=false` → `active=false`, no voice |
| 2 | `test_camera_within_range` | 800 ft → `active=true`, correct distanceFt |
| 3 | `test_camera_outside_range` | 6000 ft → `active=false` |
| 4 | `test_type_filter_speed` | `cameraTypeSpeed=false` → speed cam ignored |
| 5 | `test_type_filter_alpr` | `cameraTypeALPR=false` → ALPR ignored |
| 6 | `test_type_filter_red_light` | `cameraTypeRedLight=false` → red light ignored |
| 7 | `test_type_filter_bus_lane` | `cameraTypeBusLane=false` → bus lane ignored |
| 8 | `test_v1_alert_suppresses_camera` | V1 live → `active=false` |
| 9 | `test_v1_persisted_suppresses_camera` | V1 persisted → `active=false` |
| 10 | `test_camera_resumes_after_v1` | V1 clears, camera in range → `active=true` |
| 11 | `test_far_voice_at_1000ft` | Enter 1000 ft → far voice queued once |
| 12 | `test_far_voice_no_repeat` | Stay at 1000 ft 5s → only one announcement |
| 13 | `test_near_voice_at_500ft` | Enter 500 ft → near voice queued once |
| 14 | `test_encounter_expiry` | 10s no-see → reset. Re-approach → fresh voice. |
| 15 | `test_new_camera_resets` | Different `(latE5,lonE5)` → voice flags reset |
| 16 | `test_course_stale_shows_alert` | `courseAgeMs > 3000` → `active=true` (conservative) |

### Suite 2: `test_camera_bearing/test_camera_bearing.cpp`

| # | Test | Validates |
|---|------|-----------|
| 1 | `test_camera_directly_ahead` | bearing=90, course=90 → `true` |
| 2 | `test_camera_behind` | bearing=90, course=270 → `false` |
| 3 | `test_camera_90deg_boundary` | bearing=0, course=90 → `true` (at boundary) |
| 4 | `test_camera_91deg_past` | bearing=0, course=91.1 → `false` |
| 5 | `test_bearing_unknown` | bearing=0xFFFF → `true` |
| 6 | `test_course_invalid` | `courseValid=false` → `true` |
| 7 | `test_wraparound_359_to_1` | bearing=359, course=1 → `true` (2° diff) |
| 8 | `test_wraparound_1_to_359` | bearing=1, course=359 → `true` (2° diff) |

---

## 11. Commit Plan (7 surgical commits)

Each commit compiles & passes `ci-test.sh`.

### Commit 1: Audio clips
- `tools/generate_camera_audio.sh` — generation script
- `tools/freq_audio/mulaw/cam_speed.mul`
- `tools/freq_audio/mulaw/cam_red_light.mul`
- `tools/freq_audio/mulaw/cam_bus_lane.mul`
- `tools/freq_audio/mulaw/cam_alpr.mul`
- `tools/freq_audio/mulaw/cam_close.mul`
- Copy to `data/audio/` in the script

### Commit 2: Settings + types
- `include/camera_alert_types.h` — enum, labels
- `src/settings.h` — 10 new fields + defaults
- `src/settings.cpp` — `SD_BACKUP_VERSION` 9→10
- `src/settings_nvs.cpp` — NVS load/save for 10 keys
- `src/settings_backup.cpp` — JSON write
- `src/settings_restore.cpp` — JSON read

### Commit 3: Camera alert module (log-only)
- `src/modules/camera/camera_alert_module.h`
- `src/modules/camera/camera_alert_module.cpp`
- `process()` returns `CameraAlertState` but no display/audio wiring
- Serial.printf log when camera detected (for validation)
- Main loop integration point (module instantiation + `process()` call)

### Commit 4: Display rendering
- `src/display.h` — `updateCamera()` declaration
- `src/display.cpp` — `updateCamera()` implementation (freq area + arrow)
- `src/modules/display/display_pipeline_module.cpp` — IDLE path: camera
  check before resting display
- `src/modules/display/display_orchestration_module.h` — `cameraAlertActive`
  field in refresh context
- `src/modules/display/display_orchestration_module.cpp` — lightweight
  refresh gate

### Commit 5: Audio wiring
- `src/audio_voice.cpp` — `play_camera_voice()` function
- `src/modules/camera/camera_alert_module.cpp` — enable voice calls
  (remove log-only guard)

### Commit 6: Web UI + API
- `src/wifi_api_cameras.h` — API handler declarations
- `src/wifi_api_cameras.cpp` — GET/POST `/api/cameras/settings`,
  GET `/api/cameras/status`
- `src/wifi_manager.cpp` — register camera API routes
- `interface/src/routes/cameras/+page.svelte` — settings page
- `interface/src/routes/+layout.svelte` — nav link
- `scripts/check_wifi_api_contract.py` — add `/api/cameras/` prefixes
- Update contract snapshot

### Commit 7: Tests
- `test/test_camera_alert_module/test_camera_alert_module.cpp` — 16 tests
- `test/test_camera_bearing/test_camera_bearing.cpp` — 8 tests
- Any mock additions needed in `test/mocks/`

---

## 12. Resolved Blockers

| # | Blocker | Resolution |
|---|---------|-----------|
| 1 | Flag bitmask risk | Proven discrete IDs `{1,2,3,4}` via DB queries. Zero records outside set. |
| 2 | Voice anti-repeat | `(latE5,lonE5)` encounter key + one-shot flags + 10s expiry. No global cooldown needed. |
| 3 | Lightweight refresh conflict | `cameraAlertActive` bool in refresh context, skip frequency clear when true. |
| 4 | Course freshness | `CAMERA_COURSE_MAX_AGE_MS = 3000` in camera module. Conservative fallback: show alert when stale. |
| 5 | Under-specified tests | 24 concrete test cases across 2 suites. WiFi contract update specified. |

---

## 13. Risk Assessment

| Risk | Mitigation |
|------|-----------|
| `nearestCamera()` hot path cost | Already benchmarked ~10µs. 3×3 grid search in PSRAM. No concern. |
| GPS snapshot frequency | One `snapshot()` call per frame in camera module. Same pattern as lockout. |
| Audio task contention | `audio_playing` atomic exchange prevents overlap. Camera voice is best-effort (tier 4). |
| Display flicker | Camera only touches `dirty.frequency` + `dirty.arrow`. No full redraws. |
| Settings migration | `SD_BACKUP_VERSION` bump handles new fields. Missing keys → defaults on restore. |
