# V1 Gen2 Simple Display - Technical Manual

> This is the single authoritative and reference document for the project.
> For perf thresholds see `PERF_SLOS.md`. For the full REST API see `API.md`.
> For the GPS road-map binary format see `ROAD_MAP_FORMAT.md`.


**Version:** 4.0.0-dev  
**Hardware:** Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640×172 AMOLED)  
**Last Updated:** March 2026

---

## Release Notes

Feature-by-feature release history is maintained in `CHANGELOG.md`.

Current train (`v4.0.0-dev`) highlights:
- GPS lockout runtime stack (enforcer + learner + store/index) and lockout configuration controls.
- SD-backed perf CSV snapshots (`/perf/perf_boot_<id>.csv`) and runtime metrics correlation.
- Settings backup/restore hardening and display/runtime stability fixes.


---

## Table of Contents

1. [Quick Start](#a-quick-start) (includes Windows setup)
2. [Overview](#b-overview)
3. [System Architecture](#c-system-architecture)
4. [Boot Flow](#d-boot-flow)
5. [Bluetooth / Protocol](#e-bluetooth--protocol)
6. [Display / UI](#f-display--ui)
7. [Voice Alerts / Audio](#f2-voice-alerts--audio-system)
8. [Storage](#g-storage)
9. [Web UI Pages](#g2-web-ui-pages)
10. [Auto-Push System](#h-auto-push-system)
11. [Configuration & Build Options](#i-configuration--build-options)
12. [Troubleshooting](#j-troubleshooting)
13. [Developer Guide](#k-developer-guide)
14. [Reference](#l-reference)
15. [Testing & Validation](#m-testing--validation) (authoritative)
16. [Known Limitations](#known-limitations--todos)
17. [Known Issues / Risks](#known-issues--risks)

---

## A. Quick Start

> ⚠️ **Before updating firmware:** Download a backup of your settings from the Settings page (`/settings` → Download Backup). This preserves your colors, profiles, and configuration.

### Web Installer (Easiest)

No development tools needed — just a Chrome browser and USB cable:

👉 **[Install via Web](https://ajmdroid.github.io/v1g2_simple/)**

1. Put device in bootloader mode (hold POWER + GEAR while plugging in USB)
2. Click "Install V1-Simple" and select your device
3. Wait for install to complete, then press RESET

### Build from Source

```bash
# 1. Clone repository
git clone https://github.com/ajmdroid/v1g2_simple
cd v1g2_simple

# 2. Build and flash everything (recommended)
./build.sh --all

# The script auto-detects your OS and selects the correct environment.
# This single command:
# - Builds web interface (npm install + npm run build)
# - Deploys web assets to data/
# - Builds firmware
# - Uploads filesystem (LittleFS)
# - Uploads firmware
# - Opens serial monitor
```

**Windows users:** See the [Windows Setup](#windows-setup) subsection below for detailed instructions.

**Alternative (manual steps):**
```bash
# Build web interface
cd interface && npm install && npm run build && npm run deploy && cd ..

# Build and upload firmware + filesystem
pio run -e waveshare-349 -t uploadfs
pio run -e waveshare-349 -t upload

# Monitor serial output
pio device monitor -b 115200
```

### Verify Installation

1. Device shows boot splash (if power-on reset)
2. Screen displays "SCAN" with resting animation
3. Long-press **BOOT** (~4s) to start the WiFi AP (off by default)
4. Connect to WiFi AP: **V1-Simple** / password: **setupv1g2**
5. Browse to `http://192.168.35.5`
6. Web UI should load (SvelteKit-based interface)

### Windows Setup

Complete Windows-specific walkthrough for building and flashing.

**Prerequisites:**
- Waveshare ESP32-S3-Touch-LCD-3.49 + USB-C data cable
- Windows PC with administrator rights

**1. Install required software:**

1. **Git for Windows** (with Git Bash + Unix tools): https://git-scm.com/download/win
   - Enable: "Open Git Bash here", "Associate .sh files with Bash", Git LFS
   - Line endings: "Checkout Windows-style, commit Unix-style"
   - Terminal: MinTTY (MSYS2 default)
   - Verify: `git --version && bash --version && gzip --version`

2. **Node.js 18+**: https://nodejs.org/ — accept defaults, do NOT check "install necessary tools"
   - **Restart your computer** after install (PATH changes need it)
   - Verify: `node -v && npm -v`
   - If `node: command not found`: add `C:\Program Files\nodejs` to System PATH manually

3. **VS Code**: https://code.visualstudio.com/

4. **PlatformIO IDE extension**: VS Code → Extensions → "PlatformIO IDE" → Install
   - Wait 5-10 min for background toolchain download before proceeding

**2. Clone and build:**

```bash
git clone https://github.com/ajmdroid/v1g2_simple
cd v1g2_simple
code .
```

Install web UI deps (one-time):
```bash
cd interface && npm install && cd ..
```

**3. Configure PlatformIO terminal to use Git Bash:**
1. Click dropdown (⌄) next to `+` in terminal panel → "Select Default Profile" → "Git Bash"
2. Close all open terminals
3. Open PlatformIO CLI terminal (`Ctrl+Shift+P` → "PlatformIO: New Terminal")

**4. Connect device:**
- Plug in via USB-C. Check Device Manager → Ports for `USB Serial Device (COMx)`.
- If no COM port: try different cable, or install [Espressif USB driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/establish-serial-connection.html).
- List devices: `pio device list`

**5. Build and flash:**
```bash
./build.sh --all                    # Auto-detects Windows
./build.sh --all --upload-port COM6 # If multiple USB devices connected
```

First build downloads ~500MB of toolchain — this takes 5-15 minutes. Subsequent builds: 30-60 seconds.

**6. Common Windows pitfalls:**

| Problem | Fix |
|---------|-----|
| `./build.sh: command not found` | Use Git Bash, not PowerShell/CMD |
| `gzip` not found | Reinstall Git for Windows with default components |
| `pio: command not found` | Open PlatformIO terminal from VS Code |
| `npm: command not found` | Restart PC after Node.js install |
| No COM port | Try different USB cable; install Espressif driver |
| Build hangs downloading | First build fetches deps; wait and retry |

**7. Re-run after changes:**
```bash
./build.sh -u -m           # Firmware-only change
./build.sh -f -m           # Web UI change only
./build.sh --clean --all   # Clean build
```

**Workspace cleanup vs build cleanup:**
```bash
./build.sh --clean                 # Only rebuild artifacts managed by build.sh
python3 scripts/clean_workspace.py --safe
python3 scripts/clean_workspace.py --safe --apply
python3 scripts/clean_workspace.py --deep --apply
```

Use `clean_workspace.py --safe --apply` to remove generated validation outputs,
old mutation workspaces, release leftovers, and other local clutter without
touching tracked source. Use `--deep` only when you also want to drop heavier
caches like `.pio/`, `interface/node_modules/`, `.scratch/`, and `data/`.

**8. Factory reset (Windows):**
```bash
"$HOME/.platformio/penv/Scripts/python.exe" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --port COM4 erase_flash
./build.sh --all
```

All platforms use the single `waveshare-349` environment. `build.sh` auto-detects Windows for PlatformIO path resolution only.

**Source:** [platformio.ini](platformio.ini#L1-L50), [build.sh](build.sh#L1-L30), [interface/scripts/deploy.js](interface/scripts/deploy.js#L1-L30)

---

## B. Overview

### What This Project Does

A touchscreen remote display for the Valentine One Gen2 radar detector. Connects via Bluetooth Low Energy (BLE) to show:

- **Radar alerts:** Band (X, K, Ka, Laser), signal strength (0-6 bars), direction (front/side/rear)
- **Frequency display:** 7-segment style showing detected frequency in GHz
- **Mute control:** Tap screen to mute/unmute active alerts
- **Profile management:** 3-slot auto-push system for V1 settings profiles
- **BLE proxy:** Allows companion apps to connect through this device

### Supported Hardware

| Component | Specification |
|-----------|---------------|
| Board | Waveshare ESP32-S3-Touch-LCD-3.49 |
| CPU | ESP32-S3 @ 240MHz, 8MB or 16MB Flash, 8MB PSRAM |
| Display | AXS15231B QSPI AMOLED, 640×172 pixels |
| Touch | Integrated AXS15231B capacitive touch |
| Storage | LittleFS (internal), SD card (optional) |
| Battery | Optional LiPo via TCA9554 power management |

**Note:** Some units ship with 8MB flash. All environments use the same `partitions_v1.csv` partition table.

**Source:** [platformio.ini](platformio.ini#L1-L50), [include/config.h](include/config.h#L1-L20), [src/battery_manager.h](src/battery_manager.h#L1-L30)

### Key Features

1. **BLE Client:** Connects to V1 Gen2 (device names starting with "V1G" or "V1-")
2. **BLE Server (Proxy):** Advertises as "V1C-LE-S3" for companion app compatibility
3. **Tap-to-Mute:** Single/double tap during alert toggles mute
4. **Triple-Tap Profile Cycle:** Switch between 3 auto-push slots when idle
5. **Web Configuration:** AP mode at 192.168.35.5 for settings
6. **Full Color Customization:** Per-element RGB565 colors via web UI

**Source:** [src/main.cpp](src/main.cpp), [src/ble_client.cpp](src/ble_client.cpp)

---

## C. System Architecture

### Module Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         main.cpp                                │
│  - setup() / loop() entry points                                │
│  - FreeRTOS queue management                                    │
│  - Touch handling and mute logic                                │
│  - Auto-push state machine                                      │
└───────────┬─────────────────────────────────────────────────────┘
            │
    ┌───────┴───────┬───────────────┬───────────────┬─────────────┐
    │               │               │               │             │
┌───▼───┐     ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐ ┌─────▼─────┐
│ BLE   │     │  Packet   │   │  Display  │   │  WiFi     │ │ Settings  │
│Client │     │  Parser   │   │  Driver   │   │ Manager   │ │ Manager   │
└───────┘     └───────────┘   └───────────┘   └───────────┘ └───────────┘
```

### Source Files

> **Note:** Many original monolithic files have been split for maintainability.
> Totals reflect all split files combined.

| File(s) | Lines | Purpose |
|---------|-------|---------|
| `main.cpp` + `main_boot.cpp` + `main_loop_phases.cpp` + `main_persist.cpp` | ~2310 | Application entry, loop, boot sequence, loop phase routing, persistence helpers |
| `ble_client.cpp` + `ble_commands.cpp` + `ble_connection.cpp` + `ble_proxy.cpp` + `ble_runtime.cpp` | ~2839 | NimBLE client/server, V1 connection, proxy |
| `display.cpp` + 11 display_*.cpp files | ~4377 | Arduino_GFX drawing, segments, cards, status bar, frequency, etc. |
| `wifi_manager.cpp` + `wifi_routes.cpp` + `wifi_runtimes.cpp` + `wifi_client.cpp` | ~2346 | WebServer, API route registration, runtime routes, client mode |
| `audio_beep.cpp` + `audio_voice.cpp` | ~1311 | ES8311 DAC, I2S audio, voice alerts, SD clip playback |
| `settings.cpp` + `settings_backup.cpp` + `settings_nvs.cpp` + `settings_setters.cpp` + `settings_restore.cpp` | ~2538 | Preferences (NVS) storage, backup/restore, setters |
| `v1_profiles.cpp` | ~782 | Profile JSON on SD/LittleFS |
| `battery_manager.cpp` | ~623 | ADC, TCA9554 I/O expander |
| `packet_parser.cpp` + `packet_parser_alerts.cpp` | ~883 | ESP packet framing and decoding |
| `storage_manager.cpp` | ~118 | SD/LittleFS mount abstraction |
| `touch_handler.cpp` | ~178 | AXS15231B I2C touch polling |
| `src/modules/` (68 .cpp files, 15 dirs) | ~17k | Runtime modules for GPS, lockout, display pipeline, voice, power, WiFi API services, etc. |
| `perf_metrics.cpp` | ~698 | Latency tracking (ArduinoJson) |

### Data Flow

```
V1 Gen2 (BLE)
     │
     │ Notify (B2CE characteristic)
     ▼
┌─────────────────────────────────────────────────────────┐
│                      onV1Data()                         │
│                      (BLE task)                         │
└───────────┬─────────────────────────────┬───────────────┘
            │                             │
            │ IMMEDIATE                   │ Queue (64 slots)
            │ (zero latency)              │ (SPI-safe path)
            ▼                             ▼
     ┌─────────────┐               ┌─────────────┐
     │ Proxy Fwd   │               │processBLE() │
     │ (app)       │               │(main loop)  │
     └─────────────┘               └──────┬──────┘
                                          │
                                     ┌────▼────┐
                                     │ Parser  │
                                     │(framing)│
                                     └────┬────┘
                                          │
                                     ┌────▼────┐
                                     │ Display │
                                     │ update  │
                                     └─────────┘
```

**Key optimization:** Proxy forwarding uses `forwardToProxyImmediate()` directly in the BLE callback for zero-latency pass-through to the app. Display updates are queued because SPI operations cannot run in BLE callback context.

**Source:** [src/ble_proxy.cpp](src/ble_proxy.cpp#L325) (immediate proxy forward), [src/modules/ble/ble_queue_module.cpp](src/modules/ble/ble_queue_module.cpp) (BLE data queue), [src/modules/display/display_pipeline_module.cpp](src/modules/display/display_pipeline_module.cpp) (display updates)

### Threading Model

| Context | Description | Critical Operations |
|---------|-------------|---------------------|
| **Main loop** | Arduino `loop()` at ~200Hz | Display SPI, touch I2C, WiFi |
| **BLE task** | NimBLE internal task | Notifications, connection events, **proxy forwarding** |
| **FreeRTOS queue** | `bleDataQueue` (64 × ~268 bytes) | Decouples BLE callbacks from SPI (display only) |

**Key constraints:**
- SPI operations (display) must NOT occur in BLE callbacks → uses queue
- Proxy forwarding DOES run in BLE callback → zero added latency to app

**Source:** [src/main.cpp](src/main.cpp#L197) (onV1Data callback queues data), [src/ble_proxy.cpp](src/ble_proxy.cpp) (immediate proxy)

### Timing Constraints

| Operation | Timing | Source |
|-----------|--------|--------|
| Display draw minimum interval | 25ms (~40fps max) | `DISPLAY_DRAW_MIN_MS` in display_pipeline_module.h |
| Display update check | 50ms | `DISPLAY_UPDATE_MS` in config.h |
| Status serial print | 1000ms | main.cpp |
| Band grace period | 100ms | `BAND_GRACE_MS` in display_update.cpp |
| Touch debounce | 200ms | touch_handler.cpp |
| Tap window (triple-tap) | 600ms | `TAP_WINDOW_MS` in tap_gesture_module.h |

### Design Principles

1. **Feature-Based Organization** — Organize by what the code does for users, not technical layers.
2. **Single Responsibility Modules** — Each module owns ONE logical subsystem completely.
3. **Narrow Communication Interfaces** — Modules talk through small, well-defined APIs.
4. **Testable Boundaries** — Each module can be tested independently with mocks.

### Key Design Rules

1. **Modules receive dependencies via begin()** — dependency injection for testability
2. **Data flows DOWN** — main loop gets data and passes it to modules
3. **State lives in ONE place** — e.g., voice announcement state only in VoiceModule
4. **Incremental migration** — never break working functionality

### Module Responsibilities

| Module | Responsibility |
|--------|----------------|
| **AlertPersistenceModule** | Keeps alerts on-screen after they clear; provides state resets |
| **AutoPushModule** | Pushes V1 profiles on connect |
| **BleQueueModule** | Thread-safe BLE data queuing between callback and main loop |
| **ConnectionStateModule** | Manages BLE connect/disconnect display states |
| **ConnectionStateCadenceModule** | Throttles connection-state display transitions |
| **ConnectionStateDispatchModule** | Dispatches connection state processing at correct cadence |
| **ConnectionRuntimeModule** | BLE runtime state (connected, backpressure, last-rx) |
| **DebugApiService + DebugPerfFilesService** | Debug/metrics REST API + perf file listing/download |
| **DisplayPipelineModule** | Owns alert rendering, display state updates, and V1 packet processing |
| **DisplayOrchestrationModule** | Coordinates parsed-frame rendering, lightweight refresh, and early-loop display |
| **DisplayPreviewModule** | Color preview overlay lifecycle |
| **DisplayRestoreModule** | Restores display state after preview/settings overlay ends |
| **GpsRuntimeModule** | GPS ingest and fix/course/speed runtime state |
| **GpsApiService** | GPS lockout REST API endpoints |
| **Lockout stack** | Capture/observe/store/enforce/learn lockout state with best-effort persistence |
| **LockoutApiService + LockoutOrchestrationModule** | Lockout REST API + zone CRUD + pre-quiet controller |
| **PowerModule** | Battery monitoring, power button, sleep |
| **SpeedSourceSelector** | Runtime speed source arbitration (GPS + OBD policy) |
| **ObdRuntimeModule** | OBD-II BLE adapter connection state machine (scan/connect/poll) |
| **ObdBleClient** | NimBLE client for OBDLink CX adapter communication |
| **ObdApiService** | OBD REST API endpoints (status, scan, forget) |
| **SystemEventBus** | Thread-safe bounded ring buffer for cross-module event coordination |
| **ParsedFrameEventModule** | Collects parsed-frame signal from BLE queue for display orchestration |
| **PeriodicMaintenanceModule** | Rate-limited perf reporting, time saves, lockout learner ticks, persistence |
| **Loop phase modules** | `loop_connection_early`, `loop_power_touch`, `loop_pre_ingest`, `loop_settings_prep`, `loop_ingest`, `loop_display`, `loop_post_display`, `loop_runtime_snapshot`, `loop_tail`, `loop_telemetry` — each owns one phase of the main loop |
| **TouchUiModule** | Touch-based settings UI overlay |
| **TapGestureModule** | Triple-tap mute and other gestures |
| **VoiceModule** | All voice announcement decisions (priority/secondary/escalation) and cooldowns |
| **VolumeFadeModule** | Decides when to fade/restore volume for long-running alerts |
| **WifiOrchestrator** | WiFi/web server lifecycle |
| **WifiAutoStartModule** | Deferred WiFi auto-start with V1 settle gate |
| **WifiPriorityPolicyModule** | WiFi/BLE priority balancing at runtime |
| **WifiProcessCadenceModule** | Throttles WiFi processing cadence |
| **WifiRuntimeModule** | Orchestrates WiFi auto-start, processing, and visual sync per loop |
| **WifiVisualSyncModule** | WiFi indicator redraw debounce |
| **WiFi API services** | Settings, status, colors, profiles, devices, backup, time, autopush, control, portal, client REST endpoints |

### Dependency Injection Patterns

The codebase uses three patterns. Choose based on the module's coupling needs.

**Pattern 1: Direct Pointer Injection** (preferred for data dependencies)

Dependencies passed as pointers in `begin()`:

```cpp
class DisplayPipelineModule {
public:
    void begin(V1Display* display, PacketParser* parser, SettingsManager* settings, ...);
private:
    V1Display* display = nullptr;
};
```

Use when the module needs to read state or call methods on dependencies. Examples: `DisplayPipelineModule`, `VoiceModule`, `TapGestureModule`.

**Pattern 2: Callback Injection** (hybrid — pointers + `std::function<>`)

For actions that cross architectural boundaries:

```cpp
class TouchUiModule {
public:
    struct Callbacks {
        std::function<bool()> isWifiSetupActive;
        std::function<void()> stopWifiSetup;
        std::function<void()> startWifi;
    };
    void begin(V1Display* disp, TouchHandler* touch, SettingsManager* settings, const Callbacks& cbs);
};
```

Use when module needs direct pointer access plus cross-boundary actions. Example: `TouchUiModule`.

**Pattern 3: Providers** (C function-pointers + `void*` context)

The dominant pattern for loop-phase modules. Avoids `std::function<>` heap overhead:

```cpp
class LoopConnectionEarlyModule {
public:
    struct Providers {
        ConnectionRuntimeSnapshot (*runConnectionRuntime)(void* ctx, uint32_t nowMs, ...) = nullptr;
        void* connectionRuntimeContext = nullptr;
    };
    void begin(const Providers& hooks);
};
```

Use for loop-phase orchestration with narrow function interfaces. Examples: all `Loop*Module` types, `WifiRuntimeModule`.

| Scenario | Pattern | Reason |
|----------|---------|--------|
| Read settings, display state | Direct pointers | Need full interface |
| Toggle WiFi from touch | Callbacks | Crosses boundaries |
| Loop-phase orchestration | Providers | Narrow fn interface, no heap |

**Testing:** Direct pointers → mock classes. Callbacks → test lambdas. Providers → static functions with context capture.

---

## D. Boot Flow

### Initialization Sequence

```
1. delay(100)                          // USB stabilize
2. Create bleDataQueue (64 slots)      // FreeRTOS queue (via BleQueueModule)
3. Serial.begin(115200)
4. batteryManager.begin()              // CRITICAL: Latch power early
5. display.begin()                     // QSPI init, canvas allocation
6. Check esp_reset_reason()            // Decide splash vs skip
7. IF power-on: showBootSplash(400ms)
8. showScanning()                      // "SCAN" text
9. settingsManager.begin()             // Load from NVS
10. storageManager.begin()             // Mount SD or LittleFS
11. v1ProfileManager.begin()           // Profile filesystem access
12. (WiFi **not** auto-started)        // Long-press BOOT (~4s) to start AP when needed
13. touchHandler.begin()               // I2C touch init
14. bleClient.initBLE()                // NimBLE stack
15. bleClient.begin()                  // Start scanning
16. loop()                             // Main application loop
```

**Source:** [src/main.cpp](src/main.cpp#L347) (setup function)

### Boot Splash Logic

- **Power-on reset (`ESP_RST_POWERON`):** Show 640×172 logo with firmware version for 400ms
- **Software reset / upload:** Skip splash for faster iteration
- **Crash restart:** Skip splash

The firmware version (e.g., "v4.0.0-dev") is displayed on the boot splash screen and in the web UI header.

**Source:** [src/main.cpp](src/main.cpp#L471) (showBootSplash call), [src/display_screens.cpp](src/display_screens.cpp) showBootSplash()

### Fallback Behavior

| Failure | Behavior |
|---------|----------|
| Display init fails | Serial error, infinite delay loop |
| BLE init fails | Serial error, showDisconnected(), infinite loop |
| Touch init fails | Warning logged, continues without touch |
| Storage init fails | Warning logged, profiles disabled |
| WiFi/AP fails | Warning logged, continues without web UI |

### WiFi AP Control & Timeout

- **Default behavior:** AP is OFF by default; start it with a ~4s BOOT long-press. It stays on until you toggle it off.
- **Button toggle:** Long-press BOOT (GPIO0) for ~4s to toggle the AP on/off. Short-press enters settings mode (brightness + voice volume sliders). See [src/modules/touch/touch_ui_module.cpp](src/modules/touch/touch_ui_module.cpp).
- **Auto-timeout (optional):** Disabled by default (`WIFI_AP_AUTO_TIMEOUT_MS = 0`). Set a nonzero value in [src/wifi_manager.cpp](src/wifi_manager.cpp#L16) to allow the AP to stop after the timeout **and** at least 60s of no UI activity and zero connected stations. Timeout logic is in [WiFiManager::checkAutoTimeout()](src/wifi_manager.cpp#L493-L525), called from [WiFiManager::process()](src/wifi_manager.cpp#L528).

---

## E. Bluetooth / Protocol

### BLE UUIDs

| UUID | Purpose | Direction |
|------|---------|-----------|
| `92A0AFF4-...` | V1 Service | — |
| `92A0B2CE-...` | Display Data | V1 → Client (notify) |
| `92A0B6D4-...` | Command Write | Client → V1 |
| `92A0BAD4-...` | Alt Command Write | Fallback if B6D4 unavailable |

**Source:** [include/config.h](include/config.h#L23-L28)

### Connection State Machine

```
┌─────────────┐
│DISCONNECTED │◀────────────────────────────────────┐
└──────┬──────┘                                     │
       │ startScanning()                            │ onDisconnect()
       ▼                                            │
┌─────────────┐                                     │
│  SCANNING   │                                     │
└──────┬──────┘                                     │
       │ V1 found (name starts "V1G"/"V1-")         │
       ▼                                            │
┌─────────────┐                                     │
│SCAN_STOPPING│ (100ms settle, 200ms cold boot)     │
└──────┬──────┘                                     │
       │ scan stopped                               │
       ▼                                            │
┌─────────────┐   fail (5 attempts)   ┌─────────────┤
│ CONNECTING  │─────────────────────▶│  BACKOFF    │
└──────┬──────┘                       └─────────────┘
       │ async connect OK              (exponential)
       ▼
┌────────────────┐
│CONNECTING_WAIT │ (poll for async callback)
└──────┬─────────┘
       │ onConnect()
       ▼
┌─────────────┐
│ DISCOVERING │ (GATT discovery in background task)
└──────┬──────┘
       │ discovery done
       ▼
┌─────────────┐      ┌────────────────┐
│ SUBSCRIBING │◀────▶│SUBSCRIBE_YIELD │ (5ms yield between steps)
└──────┬──────┘      └────────────────┘
       │ all CCCDs written
       ▼
┌─────────────┐
│  CONNECTED  │
└─────────────┘
```

**Source:** [src/ble_client.h](src/ble_client.h#L30-L50) (BLEState enum), [src/ble_connection.cpp](src/ble_connection.cpp#L18) (scan callbacks), [src/ble_connection.cpp](src/ble_connection.cpp#L205) (connectToServer)

### Packet Format (ESP Protocol)

```
┌────┬──────┬────────┬──────────┬─────┬─────────────┬──────────┬────┐
│0xAA│ Dest │ Origin │ PacketID │ Len │  Payload    │ Checksum │0xAB│
│ 1B │  1B  │   1B   │    1B    │ 1B  │  Len bytes  │    1B    │ 1B │
└────┴──────┴────────┴──────────┴─────┴─────────────┴──────────┴────┘
```

| Packet ID | Name | Payload |
|-----------|------|---------|
| 0x31 | infDisplayData | 8 bytes: bands, arrows, signal, mute |
| 0x43 | respAlertData | 7+ bytes per alert: band, direction, strength, freq |
| 0x12 | respUserBytes | 6 bytes: V1 user settings |
| 0x32 | reqTurnOffDisplay | Dark mode on |
| 0x33 | reqTurnOnDisplay | Dark mode off |
| 0x34 | reqMuteOn | Mute alerts |
| 0x35 | reqMuteOff | Unmute |

**Source:** [include/config.h](include/config.h#L30-L50), [src/packet_parser.cpp](src/packet_parser.cpp#L40-L75)

### Display Data Parsing (0x31)

```cpp
// Payload byte 3 contains band/arrow/mute bitmap:
// Bit 0: Laser    Bit 4: Mute
// Bit 1: Ka       Bit 5: Front arrow
// Bit 2: K        Bit 6: Side arrow
// Bit 3: X        Bit 7: Rear arrow
```

**Source:** [src/packet_parser.cpp](src/packet_parser.cpp#L20-L37) (processBandArrow)

### Alert Data Parsing (0x43)

- First byte `& 0x0F` = alert count
- Each alert is 7 bytes in the chunked table
- Chunks accumulated until count reached, then processed

**Source:** [src/packet_parser.cpp](src/packet_parser.cpp#L170-L250)

### Signal Strength Mapping

V1 Gen2 sends raw RSSI values. Mapped to 0-8 bars using threshold tables:

```cpp
// Ka thresholds: 0x7F, 0x88, 0x92, 0x9C, 0xA6, 0xB0, 0xFF
// K thresholds:  0x7F, 0x86, 0x90, 0x9A, 0xA4, 0xAE, 0xFF
// X thresholds:  0x7F, 0x8A, 0x98, 0xA6, 0xB4, 0xC2, 0xFF
```

**Source:** [src/packet_parser_alerts.cpp](src/packet_parser_alerts.cpp#L94-L96)

### Queue / Buffering

- **Queue:** 64-slot FreeRTOS queue, each slot ~268 bytes (display path only)
- **Proxy path:** Bypasses queue entirely - `forwardToProxyImmediate()` sends directly from BLE callback
- **Overflow handling:** Drop oldest packet if full (only affects display, not proxy)
- **Buffer accumulation:** `rxBuffer` accumulates chunks until 0xAA...0xAB frame complete
- **Max buffer size:** 512 bytes, trimmed if exceeded

**Source:** [src/modules/ble/ble_queue_module.cpp](src/modules/ble/ble_queue_module.cpp) (queue management), [src/ble_client.cpp](src/ble_client.cpp) (forwardToProxyImmediate)

### Proxy Mode (App Compatibility)

When `proxyBLE=true`:
1. Device advertises as "V1-Proxy" after V1 connects
2. Companion app can connect as secondary client
3. All V1 notifications forwarded via `forwardToProxyImmediate()` - **zero added latency**
4. Commands from app forwarded to V1

**Performance:** Proxy forwarding happens directly in the BLE notification callback, before queuing for display. This ensures the app sees data with minimal latency (only the inherent V1→ESP32 BLE hop, no queuing delay).

**Source:** [src/ble_connection.cpp](src/ble_connection.cpp#L782) (notify callback), [src/ble_proxy.cpp](src/ble_proxy.cpp#L325) (forwardToProxyImmediate)

### Connection Parameters

```cpp
// NimBLE connection params: min/max interval, latency, timeout
// Optimized for low-latency proxy performance
pClient->setConnectionParams(12, 24, 0, 400);  // 15-30ms interval, 0 latency, 4s timeout
pClient->setConnectTimeout(3);  // 3 second connect timeout

// MTU set to maximum for BLE 5.x
NimBLEDevice::setMTU(517);  // 512 payload + 5 header
```

**Note:** The same tight connection parameters (15-30ms) are also applied to the app side of the proxy connection for optimal latency.

**Source:** [src/ble_connection.cpp](src/ble_connection.cpp#L295) (V1 connection params), [src/ble_proxy.cpp](src/ble_proxy.cpp) (phone connection)

### Backoff on Failure

- Base: 200ms
- Max: 1500ms
- Formula: `BACKOFF_BASE_MS * (1 << min(failures-1, 4))`
- Hard reset after 5 consecutive failures

**Source:** [src/ble_connection.cpp](src/ble_connection.cpp#L342) (backoff computation)

---

## F. Display / UI

### Screen Layout (640×172 Landscape)

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ 1.              HIGHWAY                                                      │
│ (bogey)       (profile name)                                                 │
│   VOLUME                                                                  │
│                                                                              │
│     L                                  ████                    ▲             │
│              ┌─────────┐               ████                  (front)         │
│    Ka        │ MUTED   │   ████        ████               ◀═══════▶         │
│              └─────────┘   ████        ████                  (side)          │
│     K                      ████        ████                    ▼             │
│                            ████        ████                  (rear)          │
│     X          35.500      ████        ████                                  │
│                (vol: 5)                                                    │
│             (7-seg freq)  (bars)    (bars x6)              (arrows)          │
│                                                                              │
│ [WiFi]                                                                       │
│ [Batt]                                                                       │
└──────────────────────────────────────────────────────────────────────────────┘

Layout zones (left to right):
├─ 0-80px:   Bogey counter (7-seg digit + dot) at top-left
│            Band stack (L/Ka/K/X) vertical, 24pt font
│            WiFi icon + Battery icon at bottom-left
├─ 80-400px: Profile name centered over frequency dot position
│            "MUTED" badge (when active) above frequency
│            Frequency display (7-seg "XX.XXX" or "LASER")
├─ 400-470px: Signal strength bars (6 vertical bars, colored per level)
└─ 470-640px: Direction arrows (front ▲, side ◀▶, rear ▼)
```

**Source:** [src/display_status_bar.cpp](src/display_status_bar.cpp#L135) (drawProfileIndicator), [src/display_frequency.cpp](src/display_frequency.cpp#L532) (drawFrequency), [src/display_bands.cpp](src/display_bands.cpp#L44) (drawBandIndicators), [src/display_bands.cpp](src/display_bands.cpp#L165) (drawVerticalSignalBars), [src/display_arrow.cpp](src/display_arrow.cpp#L18) (drawDirectionArrow)


### Display States

| State           | Trigger                                 | Appearance/Notes                                                                 |
|-----------------|-----------------------------------------|---------------------------------------------------------------------------------|
| Boot splash     | Power-on reset                          | Full-screen logo image                                                           |
| Scanning        | Not connected                           | "SCAN" in 7-segment                                                             |
| Resting         | Connected, no alerts                    | Dim gray segments, **arrows never shown**                                        |
| Alert           | Alert data received                     | Band color, frequency, bars, arrows (blink per V1)                              |
| Muted           | Muted alert                             | Gray color override                                                              |
| Low battery     | Critical voltage                        | Warning screen                                                                   |
| VOL 0 Warning   | Volume=0, phone app disconnect, 15s     | "VOL 0 WARNING" message for 10s, triggers app reconnect prompt                   |
| Shutdown        | Power off                               | "Goodbye" message                                                               |

**Notes:**
- **Volume Indicator:** Main volume (0-9) shown below bogey counter. If volume is 0 and phone app disconnects, triggers VOL 0 warning.
- **RSSI Indicator:** BLE signal strength shown as "V:-XXdBm" (V1 connection) and "P:-XXdBm" (Proxy connection) in top-right area. Can be hidden via Colors page.
- **Arrow Behavior:** Arrows are only shown when a valid frequency is present (never in resting state).
- **Blinking:** Band indicators and arrows blink in sync with V1 hardware (5Hz, ~100ms), using V1's image1/image2 flash bits.
- **Signal Bar Decay Reset:** On V1 disconnect, signal bar decay statics are reset to prevent stale display.
- **Alert Chunk Assembly Reset:** If alert count changes mid-transmission, chunk assembly is reset to avoid stale/mixed data.

**Source:** [src/display.h](src/display.h#L40-L55), [src/display.cpp](src/display.cpp) various show*() methods, [src/packet_parser.cpp](src/packet_parser.cpp)

### Multi-Alert Display

When multiple alerts are active simultaneously, secondary alerts appear as compact cards below the main alert:

- **Main alert:** Full-size display (frequency, 6-bar signal meter, direction arrows)
- **Secondary alerts:** Compact cards showing:
  - Band indicator (color-coded: Laser/Ka/K/X)
  - Frequency in MHz (e.g., "34712")
  - Direction arrow (front/side/rear)
  - Signal strength meter (color-coded bars matching main display)
- **Fixed layout:** Secondary row has fixed height; cards don't cause layout shifts
- **Automatic:** Mode activates when 2+ alerts are present

**Source:** [src/display_cards.cpp](src/display_cards.cpp) (drawSecondaryAlertCards)

### Color Customization

All display colors are customizable via the web UI (`/colors`). Colors are stored as RGB565 values:

| Category | Elements |
|----------|----------|
| Band Indicators | L (Laser), Ka, K, X |
| Arrows | Front, Side, Rear (separate colors) |
| Signal Bars | 6 levels (weak to strong) |
| Text | Bogey counter, frequency display |
| States | Muted alerts, persisted alerts |
| Icons | WiFi (AP mode, client connected), BLE (connected/disconnected) |
| Status | RSSI labels (V1 signal strength, Proxy signal strength), Volume indicator |

**Note:** The color picker uses a custom modal with RGB sliders for Android Chrome compatibility (native color inputs don't work reliably on mobile).

**Source:** [include/color_themes.h](include/color_themes.h#L1-L30), [interface/src/routes/colors/+page.svelte](interface/src/routes/colors/+page.svelte)

### Display Styles

| Style | Bogey Counter | Frequency | Description |
|-------|---------------|-----------|-------------|
| Classic (0) | 7-segment | 7-segment | Full retro LED-style with ghost segments |
| Serpentine (3) | 7-segment | Serpentine | Alternate style |

> **Note:** The enum also defines Modern (1) and Hemi (2), but `normalizeDisplayStyle()` in
> `src/settings.h` maps both to Classic. Only Classic and Serpentine are active at runtime.

Both active styles use 7-segment for the bogey counter to ensure proper display of the laser '=' flag and all numeric symbols.

Toggle via web UI: **Colors → Display Style**

**Source:** [src/settings.h](src/settings.h#L57-L60) (normalizeDisplayStyle), [src/display_update.cpp](src/display_update.cpp) (draw pipeline)

### 7-Segment Digit Rendering

Custom segment drawing for frequency display (e.g., "35.500"):

```cpp
constexpr bool DIGIT_SEGMENTS[10][7] = {
    // a, b, c, d, e, f, g (standard 7-segment layout)
    {true,  true,  true,  true,  true,  true,  false}, // 0
    // ... etc
};
```

- Scale factor adjusts segment size
- "Ghost" segments drawn in dim color for realism
- Decimal point handling

**Source:** [src/display_frequency.cpp](src/display_frequency.cpp) (7-segment digit rendering)

### 14-Segment Letters

For band labels (Ka, K, X, LASER):

```cpp
// Segment bit definitions for A-Z characters
// Supports: A, C, D, E, F, G, H, I, J, K, L, M, N, O, P, R, S, T, U, V, X, Y
```

**Source:** [src/display_top_counter.cpp](src/display_top_counter.cpp#L175) (draw14SegmentDigit)

### Refresh Model

1. **Throttled:** Minimum 25ms between draws (`DISPLAY_DRAW_MIN_MS`)
2. **Canvas-buffered:** Draw to `Arduino_Canvas`, then `flush()` to panel
3. **Partial updates:** `flushRegion(x, y, w, h)` sends only changed rectangles to reduce SPI traffic

**Performance path:**
- BLE notify → queue → parse → update() → flush()
- Target: <100ms end-to-end latency

**Source:** [src/modules/display/display_pipeline_module.cpp](src/modules/display/display_pipeline_module.cpp) (throttle check), [src/display.cpp](src/display.cpp) flush()

### Adding a Widget

To add a new indicator (example: speedometer):

```cpp
// 1. Add to display.h
void drawSpeedIndicator(int speed);

// 2. Implement in display.cpp
void V1Display::drawSpeedIndicator(int speed) {
    // Draw at fixed position, respect PALETTE_* colors
    FILL_RECT(x, y, w, h, PALETTE_BG);  // Clear area
    // ... draw content
}

// 3. Call from update() in display.cpp or main.cpp
display.drawSpeedIndicator(currentSpeed);
display.flush();  // Push to screen
```

**Warning:** All drawing must be from main loop context (not BLE callbacks).

---

## F2. Voice Alerts / Audio System

The display includes a built-in speaker (ES8311 DAC) that announces radar alerts when no phone app is connected.

### Hardware

| Component | Address | Purpose |
|-----------|---------|---------|
| ES8311 | 0x18 (I2C) | Audio codec / DAC |
| TCA9554 | 0x20 (I2C) | IO expander, pin 7 controls speaker amplifier |

**I2C Bus:** SDA=47, SCL=48 (shared with battery manager)
**I2S Pins:** MCLK=7, BCLK=15, WS=46, DOUT=45

**Source:** [src/audio_beep.cpp](src/audio_beep.cpp#L1-L50)

### Voice Mode Settings

The `voiceAlertMode` setting controls what information is announced:

| Mode | Value | Announcement |
|------|-------|--------------|
| `VOICE_MODE_DISABLED` | 0 | Voice alerts disabled |
| `VOICE_MODE_BAND_ONLY` | 1 | Band name only ("Ka") |
| `VOICE_MODE_FREQ_ONLY` | 2 | Frequency only ("34.7") |
| `VOICE_MODE_BAND_FREQ` | 3 | Full: band + frequency ("Ka 34.7") |

**Default:** `VOICE_MODE_BAND_FREQ` (3) - full announcements

**Settings key:** `voiceMode` (uint8_t, stored in Preferences)

**Migration:** Old `voiceAlerts` boolean is automatically migrated: `true` → `VOICE_MODE_BAND_FREQ`, `false` → `VOICE_MODE_DISABLED`

### Priority Alert Announcements

Voice alerts for the priority (strongest) alert trigger when:
1. **voiceAlertMode** is not `VOICE_MODE_DISABLED`
2. **No phone app is connected** (BLE proxy has no subscribers)
3. **Alert is not muted** on the V1
4. **Priority alert changes:**
   - **New frequency:** Full announcement based on voiceAlertMode
   - **Direction change only:** Direction-only announcement ("ahead", "behind", "side")
   - **Bogey count change:** Direction + new count (if announceBogeyCount enabled)
5. **Cooldown passed** (5 seconds since last announcement)

Announcement format examples (with `VOICE_MODE_BAND_FREQ`):
- New alert: `"Ka 34.712 ahead"` (band, frequency in GHz, direction)
- Multiple bogeys: `"Ka 34.712 ahead 2 bogeys"` (includes count when > 1)
- Direction change: `"behind"` (direction only, same alert moved)
- Laser alert: `"Laser ahead"` (no frequency, always includes direction when enabled)

**Source:** [src/modules/voice/voice_module.cpp](src/modules/voice/voice_module.cpp) (voice alert logic)

### Secondary Alert Announcements

When enabled, non-priority alerts are announced after the priority stabilizes:

**Settings:**
- `announceSecondaryAlerts` - Master toggle (default: false)
- `secondaryLaser` - Announce secondary Laser alerts (default: true)
- `secondaryKa` - Announce secondary Ka alerts (default: true)
- `secondaryK` - Announce secondary K alerts (default: false)
- `secondaryX` - Announce secondary X alerts (default: false)

**Timing:**
- Wait 1 second after priority alert stabilizes
- Wait additional 1.5 seconds since last voice alert
- Announce each qualifying secondary alert once

**Smart Threat Escalation:**
When a secondary alert ramps up from weak to strong, a full announcement provides context:
- **Trigger conditions (all must be met):**
  - Signal was ever weak (≤2 bars) at any point since detection
  - Signal is now strong (≥4 bars)
  - Strong signal sustained for at least 500ms (filters multipath spikes)
  - Total bogeys ≤4 (skips noisy environments like shopping centers)
  - Not already announced for escalation
  - Not muted by V1
  - Not laser (laser is handled by priority detection)
- **Announcement format:** `"[Band] [freq] [direction] [N] bogeys, [X] ahead, [Y] behind"`
- **Example:** `"K 24.150 behind 2 bogeys, 1 ahead, 1 behind"`
- The "was weak" flag is permanent - even if you're stopped for 60+ seconds at 1 bar, then drive toward the source, escalation will trigger when it ramps to 4+ bars
- Direction breakdown always included for situational awareness

**Source:** [src/modules/voice/voice_module.cpp](src/modules/voice/voice_module.cpp) (secondary alert logic)

### Audio Files

Pre-recorded TTS clips stored as mu-law encoded files in LittleFS (`data/audio/*.mul`), 22kHz sample rate:

| Category | Files | Example |
|----------|-------|---------|
| Band names | 4 | `band_ka.mul`, `band_k.mul`, `band_x.mul`, `band_laser.mul` |
| Directions | 3 | `dir_ahead.mul`, `dir_behind.mul`, `dir_side.mul` |
| Digits | 10 | `digit_0.mul` through `digit_9.mul` |
| Number words | 100 | `tens_00.mul` through `tens_99.mul` |
| GHz prefixes | 0 dedicated | Reuses `tens_10.mul`, `tens_24.mul`, `tens_33.mul`, `tens_34.mul`, `tens_35.mul`, `tens_36.mul` |
| Bogey count | 1 | `bogeys.mul` - spoken "bogeys" for multi-alert announcements |
| Special | 1 | `vol0_warning.mul` - "Warning, volume zero" |

**Audio format:** 8-bit mu-law encoded, 22kHz mono, loaded from LittleFS at runtime.

**Source:** [data/audio/](data/audio/) (audio files), [src/audio_beep.cpp](src/audio_beep.cpp#L400-L500) (playback)

### Volume Control

**ES8311 Register 0x32** controls DAC volume:
- 0x00 = Mute
- 0x90-0xBF = Usable range (~-24dB to 0dB)

Volume mapping: `0% = mute, 1-100% maps to 0x90-0xBF`

**Adjustment:** Short-press BOOT → use bottom blue slider → release to hear test voice ("Ka ahead")

**Source:** [src/audio_beep.cpp](src/audio_beep.cpp#L270-L290) (audio_set_volume)

### Audio API Functions

```cpp
// Full frequency announcement: "Ka 34.712 ahead 2 bogeys"
void play_frequency_voice(AlertBand band, uint16_t freqMhz, AlertDirection dir,
                          VoiceAlertMode mode, bool includeDirection, uint8_t bogeyCount);

// Band-only announcement: "Ka", "Laser"
void play_band_only(AlertBand band);

// Direction-only announcement: "ahead", "behind", "side" (optional bogey count)
void play_direction_only(AlertDirection dir, uint8_t bogeyCount = 0);

// Bogey breakdown: "2 bogeys, 1 ahead, 1 behind" (for simple breakdown)
void play_bogey_breakdown(uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side);

// Threat escalation: "K 24.150 behind 2 bogeys, 1 ahead, 1 behind" (full context)
void play_threat_escalation(AlertBand band, uint16_t freqMHz, AlertDirection direction,
                            uint8_t total, uint8_t ahead, uint8_t behind, uint8_t side);

// Set volume (0=mute, 1-100 maps to DAC range)
void audio_set_volume(int level);

// Initialize ES8311 DAC and I2S
bool audio_init();
```

**Source:** [src/audio_beep.h](src/audio_beep.h#L1-L60)

### Settings Screen

The settings screen shows two sliders:
1. **Top (green):** Display brightness (0-255)
2. **Bottom (blue):** Voice volume (0-100%)

On volume slider release, plays "Ka ahead" test clip after 1 second delay.

**Source:** [src/display_sliders.cpp](src/display_sliders.cpp#L14) (showSettingsSliders)

### Web Configuration

Voice alerts can be enabled/disabled at `/audio`:
- **Enable Voice Alerts:** Master toggle
- **Mute Voice at Volume 0:** Skip announcements when V1 volume is 0

**API:** `POST /api/audio/settings` with `voiceAlertMode=0|1|2|3`

### Volume Fade (V1 Alert Volume Reduction)

Automatically reduces V1's alert volume after the initial announcement period. Useful for long alerts where you've acknowledged the threat but don't want continuous loud beeping.

**Settings:**
- `alertVolumeFadeEnabled` - Master toggle (default: false)
- `alertVolumeFadeDelaySec` - Seconds at full volume before fading (1-10, default: 2)
- `alertVolumeFadeVolume` - Target volume to fade to (0-9, default: 1)

**Behavior:**
1. Alert starts → V1 alerts at normal volume
2. After delay period → Volume reduced to target level
3. Alert clears → Volume restored to original
4. User mutes on V1 → Volume restored, tracking resets (V1 mute is always honored)

**Smart New Threat Detection:**
- System tracks all frequencies seen during a fade session (up to 12)
- **New frequency appears** → Volume restored to full, fade timer restarts
- **Priority shuffles between known frequencies** → Stays faded (no flip-flop)
- Example: 35.501 and 35.515 both faded, new 35.490 appears → full volume for new threat

**NVS Keys:** `volFadeEn`, `volFadeSec`, `volFadeVol`

**Configure:** Web UI at `/audio` → "Volume Fade" section

---

## G. Storage

### Filesystem Hierarchy

```
/                           (LittleFS - 4MB partition)
├── index.html              Web UI entry
├── settings.html           Settings page
├── devices.html            Device management
├── profiles.html           Profile management
├── colors.html             Color customization
├── autopush.html           Auto-push configuration
├── _app/                   SvelteKit build artifacts
│   ├── env.js
│   ├── version.json
│   └── immutable/
│       ├── assets/*.css
│       ├── chunks/*.js
│       ├── entry/*.js
│       └── nodes/*.js
└── v1settings_backup.json  Settings backup (if SD unavailable)

/sdcard/                    (SD card - optional)
├── profiles/               V1 user profiles (JSON)
│   ├── Default.json
│   ├── Highway.json
│   └── ...
└── v1settings_backup.json  Settings backup
```

### Settings Storage (NVS)

ESP32 Preferences API with namespace `v1settings`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| enableWifi | bool | true | WiFi enabled |
| wifiMode | int | 2 (AP) | V1_WIFI_OFF/STA/AP/APSTA |
| apSSID | String | "V1-Simple" | AP network name |
| apPassword | String | "setupv1g2" (obfuscated) | AP password |
| proxyBLE | bool | true | BLE proxy enabled |
| proxyName | String | "V1-Proxy" | Proxy advertised name |
| brightness | uint8 | 200 | Display brightness 0-255 |
| voiceVol | uint8 | 75 | Voice alert volume 0-100% |
| voiceMode | uint8 | 3 | Voice mode: 0=off, 1=band, 2=freq, 3=band+freq |
| voiceDir | bool | true | Include direction in voice announcements |
| bogeyCount | bool | true | Announce bogey count ("2 bogeys") |
| muteVoiceZero | bool | false | Mute voice when V1 volume is 0 |
| secAlerts | bool | false | Announce secondary (non-priority) alerts |
| secLaser | bool | true | Announce secondary Laser alerts |
| secKa | bool | true | Announce secondary Ka alerts |
| secK | bool | false | Announce secondary K alerts |
| secX | bool | false | Announce secondary X alerts |
| autoPush | bool | true | Auto-push on connect |
| activeSlot | int | 0 | Active profile slot 0-2 |
| slot0prof | String | "" | Slot 0 profile name |
| slot0mode | int | 0 | Slot 0 V1 mode |
| slot0dark | bool | false | Slot 0 dark mode |
| slot0mz | bool | false | Slot 0 mute-to-zero |
| slot0persist | uint8 | 0 | Slot 0 alert persistence (0-5 sec) |
| slot0prio | bool | false | Slot 0 priority arrow only |
| lastV1Addr | String | "" | Last connected V1 address |
| hideWifi | bool | false | Hide WiFi icon |
| hideProf | bool | false | Hide profile indicator |
| hideBatt | bool | false | Hide battery icon |
| hideBle | bool | false | Hide BLE icon |
| hideVol | bool | false | Hide volume indicator |
| freqBandCol | bool | false | Use band color for frequency display |
| colorArrF | uint16 | theme | Front arrow color |
| colorArrS | uint16 | theme | Side arrow color |
| colorArrR | uint16 | theme | Rear arrow color |
| colorBleC | uint16 | 0x07E0 | BLE connected color |
| colorBleD | uint16 | 0x001F | BLE disconnected color |

*Note: Slot 1 and 2 have analogous keys (slot1dark, slot2persist, etc.)*

**Source:** [src/settings.cpp](src/settings.cpp#L119-L340) (load), [src/settings_nvs.cpp](src/settings_nvs.cpp#L338-L470) (save)

### Password Obfuscation

WiFi passwords stored with XOR obfuscation (NOT encryption):

```cpp
static const char XOR_KEY[] = "V1G2-S3cr3t-K3y!";
// Same function for encode/decode
static String xorObfuscate(const String& input) {
    // XOR each character with rotating key
}
```

**Security note:** This prevents casual viewing but is NOT secure against determined attackers.

**Source:** [src/settings_nvs.cpp](src/settings_nvs.cpp#L44-L55) (xorObfuscate), XOR key at [src/settings.cpp](src/settings.cpp#L75)

### Profile File Format

```json
{
  "name": "Highway",
  "description": "High sensitivity for highway driving",
  "displayOn": true,
  "settings": {
    "bytes": [255, 255, 255, 255, 255, 255]
  }
}
```

**Source:** [src/v1_profiles.h](src/v1_profiles.h#L1-L100), [src/v1_profiles.cpp](src/v1_profiles.cpp)

### SD Card Backup

On settings save, automatically backs up to `/v1settings_backup.json` on SD card.
On boot, if NVS appears empty (fresh flash), restores from SD backup.

**Source:** [src/settings_backup.cpp](src/settings_backup.cpp#L463) (backupToSD), [src/settings_backup.cpp](src/settings_backup.cpp#L672) (restoreFromSD)

---

## G2. Web UI Pages

The web interface is built with SvelteKit and daisyUI (TailwindCSS). Source is in `interface/src/routes/`.

### Page Routes

| Route | File | Purpose |
|-------|------|---------|
| `/` | `+page.svelte` | Home - connection status, quick links |
| `/settings` | `settings/+page.svelte` | WiFi AP, BLE proxy settings |
| `/audio` | `audio/+page.svelte` | Voice alert settings, volume fade |
| `/colors` | `colors/+page.svelte` | Color customization |
| `/autopush` | `autopush/+page.svelte` | Auto-push slot configuration |
| `/profiles` | `profiles/+page.svelte` | V1 profile management |
| `/devices` | `devices/+page.svelte` | Known V1 device management |
| `/lockouts` | `lockouts/+page.svelte` | GPS lockout zone management and observation log |
| `/integrations` | `integrations/+page.svelte` | GPS and external integration settings |
| `/dev` | `dev/+page.svelte` | Debug tools: metrics, perf files, V1 scenarios, panic log |

### Settings Page (`/settings`)

Controls:
- **AP Name/Password:** Change WiFi network name and password (AP-only, no station mode)
- **BLE Proxy:** Enable/disable app forwarding
- **Proxy Name:** Advertised BLE name (default: "V1C-LE-S3")
- **Device Time:** Read-only runtime clock snapshot sourced by the device (GPS/SNTP/RTC when available)

**Backup & Restore:**
- **Download Backup:** Export all settings (colors, slot configs, voice settings) and V1 profiles to a JSON file
- **Restore from Backup:** Upload a previously saved backup file to restore all settings and profiles

**Source:** [interface/src/routes/settings/+page.svelte](interface/src/routes/settings/+page.svelte)

### Audio Page (`/audio`)

Controls:
- **Voice Content:** Dropdown to select announcement style (Disabled / Band Only / Frequency Only / Band + Frequency)
- **Include Direction:** Toggle to append "ahead", "side", or "behind" to announcements
- **Announce Bogey Count:** Toggle to append "2 bogeys", "3 bogeys" when multiple alerts active
- **Mute Voice at Volume 0:** Skip voice alerts when V1 volume is set to 0 (warning still plays)

**Secondary Alert Announcements:**
- **Announce Secondary Alerts:** Master toggle for non-priority alert announcements
- **Per-band filters:** When secondary enabled, choose which bands to announce (Laser, Ka, K, X)

**Volume Fade (V1 Alert Volume):**
- **Enable Volume Fade:** Reduce V1 alert volume after initial announcement period
- **Delay:** Seconds to wait at full volume before fading (1-10)
- **Reduced Volume:** Target volume to fade to (0-9)

**Speaker Volume:** Slider to control ES8311 DAC output level (0-100%)

Voice alerts announce through the built-in speaker when no phone app is connected via BLE proxy. Priority alerts are announced immediately; secondary alerts wait for priority to stabilize. Smart threat escalation detects when secondary alerts ramp up from weak (≤2 bars) to strong (≥4 bars sustained) and announces with full context.

**Source:** [interface/src/routes/audio/+page.svelte](interface/src/routes/audio/+page.svelte)

### GPS / Lockout Settings (`/integrations` and `/lockouts`)

Controls:
- **GPS Module:** Enable/disable GPS for location-based features (auto-detects within 60s)

**Auto-Lockout Settings:**
- **Enable Auto-Lockout:** Master toggle for automatic false alert learning
- **Ka Protection:** Never auto-learn Ka band (real threats, default: on)
- **Frequency Tolerance:** MHz tolerance for lockout matching (default: 10)
- **Learn Count:** Hits needed to promote to lockout (default: 3; moving alerts still need 4)
- **Unlearn Count:** Passes without alert to demote auto-lockout (default: 0, legacy decay)
- **Manual Delete Count:** Passes to demote manual lockouts (default: 0, never auto-delete)
- **Learn Interval:** Hours between counted hits (default: 0, disabled)
- **Unlearn Interval:** Hours between counted misses (default: 0, disabled)

**Source:** [interface/src/routes/integrations/+page.svelte](interface/src/routes/integrations/+page.svelte), [interface/src/routes/lockouts/+page.svelte](interface/src/routes/lockouts/+page.svelte)

### Colors Page (`/colors`)

Controls:
- **Display Style:** Classic (full 7-segment) or Serpentine (alternate style)
- **Custom Colors:** Per-element RGB565 colors (via custom RGB slider picker for Android compatibility)
  - Bogey counter, Frequency display
  - Individual arrow colors (Front, Side, Rear separately)
  - Band colors (L, Ka, K, X individually)
  - Signal bar gradient (6 levels)
  - WiFi icon colors (AP mode, Client connected)
  - BLE icon colors (Connected, Disconnected states)
  - RSSI label colors (V1 connection, Proxy connection)
  - Muted alert color, Persisted alert color
  - Volume indicator colors (Main volume, Mute volume)
- **Use Band Color for Frequency:** When enabled, frequency display uses the detected band's color instead of custom frequency color
- **Visibility Toggles:** Hide WiFi icon, Hide profile indicator, Hide battery icon, Hide BLE icon, Hide volume indicator, Hide RSSI indicator
- **Test Button:** Shows color demo on physical display (cycles through X, K, Ka, Laser with cards and muted state)

**Source:** [interface/src/routes/colors/+page.svelte](interface/src/routes/colors/+page.svelte)

### OBD Settings (within `/settings`)

Controls:
- **Enable OBD:** Master toggle — when enabled, V1-Simple scans for and connects to an OBDLink CX adapter via BLE
- **Min RSSI:** Minimum BLE signal strength for scan results (default: −80 dBm)
- **Status Display:** Shows current OBD connection state, speed reading, RSSI, poll count, and error counts (polls every 2 seconds)
- **Scan Now:** Triggers a 5-second BLE scan for nearby OBDLink devices
- **Forget Device:** Clears the saved OBD adapter address and disconnects

When OBD is enabled and connected, the speed source selector prefers OBD speed over GPS speed for lockout mute/unmute decisions.

**Source:** [interface/src/lib/features/settings/SettingsObdCard.svelte](interface/src/lib/features/settings/SettingsObdCard.svelte)

### Auto-Push Page (`/autopush`)

Controls:
- **Enable Auto-Push:** Toggle for automatic profile push on connection
- **Active Slot:** Currently selected slot (0, 1, or 2)
- **Per-Slot Configuration:**
  - Profile name (from saved profiles)
  - V1 Mode override (All Bogeys, Logic, Advanced Logic)
  - Slot display name (e.g., "DEFAULT", "HIGHWAY", "COMFORT")
  - Slot indicator color
  - Main volume (0-9, or "No Change")
  - Mute volume (0-9, or "No Change")
  - Dark mode (V1 display off when slot active)
  - Mute to zero (mute completely silences alerts)
  - Alert persistence / ghost (0-5 seconds, shows last alert in gray after it clears)
  - Priority arrow only (show only strongest alert's direction, reduces flicker with multiple alerts)
- **Quick-Push Buttons:** Activate and push a slot immediately

**Source:** [interface/src/routes/autopush/+page.svelte](interface/src/routes/autopush/+page.svelte)

### Profiles Page (`/profiles`)

Controls:
- **Profile List:** View all saved profiles
- **Create/Edit:** Full V1 user settings editor
  - Band enables (X, K, Ka, Laser)
  - Sensitivity levels per band
  - Mute behavior, bogey lock
  - Euro mode, TMF, K filter
  - Photo system settings
- **Pull from V1:** Read current V1 settings into new profile
- **Push to V1:** Send profile to connected V1
- **Delete:** Remove saved profile

**Source:** [interface/src/routes/profiles/+page.svelte](interface/src/routes/profiles/+page.svelte)

### Devices Page (`/devices`)

Controls:
- **Known Devices:** List of previously connected V1 units
- **Friendly Name:** Custom name for each V1
- **Default Profile:** Auto-push profile for specific V1
- **Delete:** Remove device from list

**Source:** [interface/src/routes/devices/+page.svelte](interface/src/routes/devices/+page.svelte)

### Adding a New Page

1. Create `interface/src/routes/newpage/+page.svelte`
2. Add route handler in [src/wifi_routes.cpp](src/wifi_routes.cpp) inside `WiFiManager::setupWebServer()`:
   ```cpp
   server.on("/newpage", HTTP_GET, [this]() { 
       serveLittleFSFile("/newpage.html", "text/html"); 
   });
   ```
3. Add API endpoints if needed
4. Rebuild: `./build.sh --all`

---

## H. Auto-Push System

The auto-push system automatically configures the V1 to a saved profile when connection is established. It provides three "slots" for quick profile switching.

### Slot System

| Slot | Default Name | Purpose |
|------|--------------|---------|
| 0 | Default | Normal driving conditions |
| 1 | Highway | High sensitivity for open roads |
| 2 | Comfort | Reduced sensitivity for urban areas |

Each slot stores:
- Profile name (references a profile in `/profiles/`)
- V1 mode override (All Bogeys, Logic, Advanced Logic)
- Volume settings (main, mute volume)
- Dark mode setting (turns off V1's display)
- Mute to zero setting (completely silences muted alerts)
- Alert persistence duration (0-5 seconds ghost display after alert clears)
- Priority arrow only (show only strongest alert's direction arrow)

**Source:** [src/settings.cpp](src/settings.cpp#L299-L340) (load), [src/settings_nvs.cpp](src/settings_nvs.cpp#L432-L462) (save)

### Auto-Push State Machine

When auto-push is enabled and V1 connects, the system executes a 5-step sequence:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                       Auto-Push State Machine                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   IDLE ──(connection established)──▶ WAIT_READY                        │
│                                         │                               │
│                                    (V1 ready)                           │
│                                         ▼                               │
│                                      PROFILE                            │
│                                    (send 6 user bytes)                  │
│                                         │ 100ms                         │
│                                         ▼                               │
│                                      DISPLAY                            │
│                                    (display on/off)                     │
│                                         │ 100ms                         │
│                                         ▼                               │
│                                       MODE                              │
│                                    (All/Logic/AdvLogic)                 │
│                                         │ 100ms                         │
│                                         ▼                               │
│                                      VOLUME                             │
│                                    (main + mute vols)                   │
│                                         │ 100ms                         │
│                                         ▼                               │
│                                       IDLE                              │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**Timing:** Each step waits 100ms before sending the next command to avoid overwhelming the V1.

**Source:** [src/modules/auto_push/auto_push_module.cpp](src/modules/auto_push/auto_push_module.cpp) (auto-push state machine)

### Debug Logging

Auto-push Serial spam is gated by a compile-time switch. To see every state transition and command:

1. Auto-push debug logs are controlled by `AUTO_PUSH_LOGF` / `AUTO_PUSH_LOGLN` macros in [src/modules/perf/debug_macros.h](src/modules/perf/debug_macros.h).
2. Toggle `n_LOGS` in that file to enable/disable.

Default is `true`.

### Profile Cycling (Touch Gesture)

**Triple-tap** the screen to cycle through profiles 0→1→2→0...

When a new slot is selected:
1. Display flashes the new profile name
2. If auto-push enabled, immediately pushes the new profile's settings to V1
3. Active slot persisted to NVS

**Source:** [src/modules/touch/tap_gesture_module.cpp](src/modules/touch/tap_gesture_module.cpp) (triple-tap profile cycle)

### Configuration (Web UI)

The `/autopush` page allows:
- Enable/disable auto-push feature
- Configure each of the 3 slots:
  - Select profile from saved profiles
  - Override V1 mode
  - Set main volume (0-9, or 255 = "No Change")
  - Set mute volume (0-9, or 255 = "No Change")
- Default: 255 (No Change) - V1 keeps its current volume unless you override

### Protocol Commands

Auto-push sends these V1 ESP commands:

| Step | Command | Payload |
|------|---------|---------|
| PROFILE | reqWriteUserBytes | 6 bytes from profile |
| DISPLAY | reqTurnOnMainDisplay / reqTurnOffMainDisplay | - |
| MODE | reqChangeMode | 1=All, 2=Logic, 3=AdvLogic |
| VOLUME | reqSetVolume | main (0-9), mute (0-9) |

**Source:** [src/ble_commands.cpp](src/ble_commands.cpp) (sendCommand, setVolume, writeUserBytes, etc.)

---

## I. Configuration & Build Options

### PlatformIO Environments

One firmware environment is used on all platforms (Mac, Linux, Windows):
- `waveshare-349` — Waveshare ESP32-S3-Touch-LCD-3.49 (16MB flash)

A separate `native` environment runs unit tests on the host machine.

```ini
[env:waveshare-349]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.11/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino

build_flags = 
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D BOARD_HAS_PSRAM
    -D DISPLAY_WAVESHARE_349=1
    -D SCREEN_WIDTH=640
    -D SCREEN_HEIGHT=172
    # QSPI pins...

lib_deps = 
    moononournation/GFX Library for Arduino@1.6.4
    h2zero/NimBLE-Arduino@2.3.7
    bblanchon/ArduinoJson@7.4.2
    https://github.com/takkaO/OpenFontRender.git

board_build.partitions = partitions_v1.csv
board_build.filesystem = littlefs
```

**Windows users:** Use the same `waveshare-349` environment — no separate Windows env is needed.
See [Section A: Windows Setup](#windows-setup) for detailed Windows instructions.

**Source:** [platformio.ini](platformio.ini#L1-L80)

### Compile-Time Flags

| Flag | Default | Description |
|------|---------|-------------|
| `DISPLAY_WAVESHARE_349` | defined | Target board selection |
| `SCREEN_WIDTH` | 640 | Display width in pixels |
| `SCREEN_HEIGHT` | 172 | Display height in pixels |
| `REPLAY_MODE` | undefined | Enable packet replay for UI testing |
| `PERF_METRICS` | 1 | Enable performance counters |
| `PERF_MONITORING` | 1 | Enable sampled timing |
| `PERF_VERBOSE` | 0 | Enable immediate latency alerts |
| `AUTOPUSH_DEBUG_LOGS` | false | Enable verbose `[AutoPush]` Serial logs (set in `src/main.cpp`) |

**Source:** [include/config.h](include/config.h#L65-L68), [src/perf_metrics.h](src/perf_metrics.h#L25-L55)

### Runtime Configuration

All settings modifiable via web UI at `http://192.168.35.5`:

- **Settings page:** WiFi AP name/password, BLE proxy
- **Audio page:** Voice alerts enable/disable
- **Colors page:** Display style, custom per-element colors
- **Profiles page:** Pull V1 settings, save as profiles
- **Auto-push page:** Configure 3 profile slots
- **Devices page:** Manage known V1 addresses

### Replay Mode

For UI development without V1 hardware:

1. Uncomment `#define REPLAY_MODE` in [include/config.h](include/config.h#L68)
2. Rebuild and flash
3. Device cycles through sample alert packets

**Source:** [src/main.cpp](src/main.cpp#L805) (replay mode packet definitions)

---

## J. Troubleshooting

### Serial Debug Output

Connect at 115200 baud. Key prefixes:

| Prefix | Source |
|--------|--------|
| `[Battery]` | battery_manager.cpp |
| `[BLE_SM]` | ble_connection.cpp state machine |
| `[SetupMode]` | wifi_manager.cpp + wifi_routes.cpp |
| `[Settings]` | settings.cpp + settings_nvs.cpp + settings_backup.cpp |
| `[AutoPush]` | modules/auto_push/auto_push_module.cpp |
| `[OBD]` | modules/obd/obd_runtime_module.cpp |
| `[Touch]` | touch_handler.cpp |

### Connection Issues

**Can't find V1-Simple WiFi network:**
1. WiFi is **off by default** — long-press BOOT (~4s) to toggle WiFi AP on/off
2. Optional: set `enableWifiAtBoot=true` in `/settings` if you need AP on every boot
3. Move closer — ESP32 WiFi range is limited to ~30 feet

**Can't connect to V1-Simple WiFi:**
1. Use correct password: default is `setupv1g2`
2. Forget and reconnect (remove saved network, reconnect fresh)
3. Disable 5GHz on your phone — some phones try 5GHz first

**V1 won't connect via BLE:**
1. Ensure V1 Bluetooth is ON (V1 Menu → Expert → Bluetooth → ON, "Visible" mode)
2. Power cycle both V1 and V1-Simple
3. Disconnect V1 from phone apps first
4. Keep V1 within 3 feet during initial pairing

**Frequent BLE disconnections:**
1. Reduce distance between V1-Simple and V1
2. Check battery — low V1-Simple battery affects BLE stability
3. Disable proxy mode if not needed (reduces disconnect risk)

### Display Problems

**Display is blank/black:**
1. Touch the display to wake it
2. Check serial for "[Display] QSPI init failed"
3. Power cycle (hold power 10 seconds)

**Display colors wrong/inverted:**
1. Reset colors: Settings → Display Colors → Reset to Default
2. If testing colors, send clear preview command

**Touch not responding:**
1. Clean screen — fingerprints can affect capacitive touch
2. Restart device — touch controller may need reset
3. Check serial for "[Touch] ERROR"

### GPS Issues

**GPS shows "No Fix":**
1. Wait longer — first fix can take 2-5 minutes (cold start)
2. Go outside — GPS needs clear sky view
3. Not all units have GPS installed

**Speed doesn't match speedometer:**
- Vehicle speedometers typically read 2-5% high — this is normal

### OBD Issues

**OBD scan finds no devices:**
1. Ensure OBDLink CX is plugged into the vehicle's OBD-II port and the ignition is on
2. Move V1-Simple closer — BLE range is limited; try lowering Min RSSI threshold
3. Confirm the adapter is an OBDLink CX (STN2120 chip) — other OBD adapters are not supported

**OBD connects but shows no speed:**
1. The vehicle must be running — PID 0x0D returns 0 km/h when stationary
2. Check serial for `[OBD]` log lines showing AT init or poll errors
3. Some vehicles respond slowly — wait 5-10 seconds after connection

**OBD disconnects frequently:**
1. Check adapter power — a loose OBD-II port connector can drop BLE
2. Reduce distance between V1-Simple and the OBD adapter
3. After 5 consecutive poll errors the module enters a 60-second backoff before reconnecting

**OBD speed not used for lockout muting:**
1. Verify OBD is enabled in Settings and the status shows "POLLING"
2. Speed readings older than 3 seconds are considered stale and ignored
3. The speed source selector prefers OBD over GPS — if OBD speed is valid, it will be used

### Auto-Lockout Issues

**Lockouts not learning:**
1. Check `gpsLockoutMode` is not set to `off`
2. Default requires 3 passes (`gpsLockoutLearnerPromotionHits`)
3. Must be within lockout learner radius and frequency tolerance
4. Ka band learning is disabled by default (`gpsLockoutKaLearningEnabled`)

**Lockouts learning too aggressively:**
1. Increase `gpsLockoutLearnerPromotionHits` (default is 3)
2. Decrease `gpsLockoutLearnerRadiusE5` for tighter geo-match
3. Decrease `gpsLockoutLearnerFreqToleranceMHz`
4. Disable Ka learning: set `gpsLockoutKaLearningEnabled` to false

### Audio Problems

**No sound:**
1. Check mute state and volume level
2. Check audio profile in Settings → Audio
3. Test speaker via debug page

**Audio distorted:**
1. Lower volume — high volume can cause clipping
2. Power cycle — audio codec may need reset

### Performance Issues

**Web UI is slow:**
1. Reduce connected clients (1-2 devices max)
2. Close unused pages (each page polls for updates)
3. Disable `/dev` auto-refresh metrics when not debugging
4. Score perf CSV: `python3 tools/score_perf_csv.py <csv> --profile drive_wifi_off --session longest-connected`

**Device restarting/crashing:**
1. Check battery — low battery can cause restarts
2. Collect diagnostics: `/api/debug/panic`, `/api/debug/metrics`, latest `/perf/perf_boot_*.csv`
3. Update firmware

### Factory Reset

Settings are stored in NVS (non-volatile storage) and persist across firmware updates.

**Method 1 — Via Web UI:**
1. Connect to device WiFi → Settings page → Factory Reset

**Method 2 — Via USB Serial (full flash erase):**

```bash
# Windows (Git Bash):
"$HOME/.platformio/penv/Scripts/python.exe" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --port COM4 erase_flash

# Mac/Linux:
~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbmodem* erase_flash

# Then re-upload everything:
./build.sh --all
```

**Port names:** Windows: `COM4`/`COM5`. Mac: `/dev/cu.usbmodem*`. Linux: `/dev/ttyACM0` or `/dev/ttyUSB0`.

**After erase:** Device boots with factory defaults (WiFi: V1-Simple/setupv1g2).

### BLE Connection Debugging

Enable verbose logging by checking serial output for:

```
[BLE_SM] SCANNING -> SCAN_STOPPING | Reason: V1 found
[BLE_SM] Connect attempt 1/3
[BLE_SM] Service discovery completed
[BLE_SM] Subscribed to display data notifications
```

**Tuning knobs (advanced):**
- Connection attempts: 5 (see MAX_CONNECT_ATTEMPTS in [src/ble_client.h](src/ble_client.h#L377)).
- Backoff/settle: adjust `BACKOFF_BASE_MS` / `BACKOFF_MAX_MS` and `SCAN_STOP_SETTLE_MS` / `SCAN_STOP_SETTLE_FRESH_MS` in [src/ble_client.h](src/ble_client.h#L440) if you need slower retries or longer radio settle time.

### Web API Quick Reference

For the full API reference with request/response schemas and examples, see [API.md](API.md).

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | BLE connection state, V1 info |
| GET | `/api/device/settings` | AP/proxy/power/dev settings |
| POST | `/api/device/settings` | Save AP/proxy/power/dev settings |
| GET | `/api/gps/config` | GPS/lockout settings |
| POST | `/api/settings` | Save GPS/lockout settings |
| GET | `/api/settings/backup` | Download settings as JSON |
| POST | `/api/settings/restore` | Restore settings from JSON |
| GET | `/api/v1/profiles` | List saved profiles |
| POST | `/api/v1/push` | Push profile to V1 |
| GET | `/api/autopush/slots` | Get slot configurations |
| GET | `/api/audio/settings` | Get current audio settings |
| POST | `/api/audio/settings` | Save audio settings |
| GET | `/api/displaycolors` | Get current colors, brightness, and display style |
| GET | `/api/obd/config` | Get OBD enable/min RSSI config |
| GET | `/api/debug/metrics` | Performance metrics |
| GET | `/ping` | Health check |

---

## K. Developer Guide

### Operational Invariants

- **BLE client lifecycle:** Never delete NimBLE clients at runtime. Reuse or
  disconnect the existing client instead.
- **Display threading:** Display updates must stay on the main loop. BLE
  callbacks may queue work, but they must not touch display SPI paths directly.
- **Battery latch timing:** `batteryManager.begin()` must run immediately after
  early boot setup so battery-powered starts do not drop the latch.
- **Radio contention:** WiFi and BLE share one radio. BLE scan duty cycle is
  tuned for ESP32-S3 discovery reliability, so WiFi work must stay non-blocking
  and coexistence-aware.
- **Logging conventions:** Use stable uppercase subsystem prefixes such as
  `[BLE]`, `[WIFI]`, and `[PERF]`; keep hot-path logs gated, and leave
  actionable failures visible.

### Repository Structure

```
v1g2_simple/
├── src/                    C++ source files
├── include/                Header files (config, themes, fonts)
├── interface/              SvelteKit web UI source
│   ├── src/routes/         Page components
│   ├── build/              npm build output
│   └── scripts/deploy.js   Copy to data/
├── data/                   LittleFS contents (web UI)
├── scripts/                Build helper scripts
├── tools/                  Development utilities
├── docs/                   Documentation
├── platformio.ini          Build configuration
└── build.sh                Full build script
```

### Coding Conventions

- **Naming:** camelCase for functions/variables, PascalCase for classes/types
- **Indentation:** 4 spaces
- **Line length:** ~100 characters soft limit
- **Comments:** Doxygen-style for public APIs
- **Includes:** Local headers with `""`, system with `<>`

### Adding a New Feature

1. **Header:** Declare in appropriate `.h` file
2. **Implementation:** Implement in corresponding `.cpp`
3. **Web API:** Add endpoint in a dedicated `wifi_*_api_service.cpp` module under `src/modules/wifi/`
4. **Settings:** Add to `settings.h`/`settings.cpp` if persistent
5. **Web UI:** Add page in `interface/src/routes/`

### Build Commands

This section is the canonical command reference. Other docs link here instead
of repeating full command blocks.

**Primary build script** (`./build.sh`):

```bash
./build.sh              # Build only (no upload)
./build.sh -u           # Build and upload firmware
./build.sh -f           # Build and upload filesystem only
./build.sh -u -m        # Build, upload firmware, open monitor
./build.sh --all        # Full build + upload filesystem + firmware + monitor
./build.sh --clean -a   # Clean build and upload everything
./build.sh --skip-web   # Skip web interface rebuild
./build.sh --help       # Show all options
```

**Manual PlatformIO commands:**

```bash
pio run -e waveshare-349                  # Build firmware only
pio run -e waveshare-349 -t upload        # Upload firmware
pio run -e waveshare-349 -t uploadfs      # Upload web filesystem
pio device monitor -b 115200              # Serial monitor
```

**Web interface development:**

```bash
cd interface
npm install                               # Install dependencies
npm run dev                               # Dev server with hot reload
npm run build                             # Production build
npm run deploy                            # Copy build/ to data/
```

**Helper scripts:**

| Script | Purpose |
|--------|---------|
| `scripts/pio-size.sh` | Report firmware size |
| `scripts/pio-check.sh` | Run static analysis |
| `tools/smoke_metrics_runtime.py` | Runtime perf counter smoke checks (API + CSV reflection) |
| `tools/score_perf_csv.py` | Score `/perf/perf_boot_*.csv` against hard/advisory SLOs |

**Source:** [build.sh](build.sh), [scripts/](scripts/)

### Testing

Automated unit tests run via PlatformIO's Unity framework:

```bash
pio test -e native          # Run all native unit tests
```

Manual testing procedure:

1. **Build test:** `./build.sh` (or `pio run -e waveshare-349`)
2. **Static analysis:** `./scripts/pio-check.sh`
3. **Size check:** `./scripts/pio-size.sh`
4. **Functional test:**
   - Power on, verify splash
   - Connect to V1, verify alerts display
   - Test tap-to-mute
   - Test triple-tap profile switch
   - Test all web UI pages
   - Test auto-push slot cycling
   - Test color customization

**Runtime metrics smoke check (API + CSV):**

```bash
python tools/smoke_metrics_runtime.py --base-url http://192.168.35.5 --profile power_safe
```

- Uses `/api/debug/metrics` to verify target counters increment.
- Uses `/api/debug/perf-files` + `/api/debug/perf-files/download` to confirm CSV reflection.
- Use `--profile power_full` only when intentionally testing shutdown-producing counters.

### Performance-Sensitive Paths

**⚠️ Hot paths - avoid blocking:**

1. `onV1Data()` - BLE callback, forwards to proxy immediately then queues for display
2. `forwardToProxyImmediate()` - Called in BLE callback, must complete fast (~1ms)
3. `BleQueueModule::process()` - Main loop, target <10ms
4. `display.update()` + `flush()` - Target <15ms total

**Source:** [src/ble_connection.cpp](src/ble_connection.cpp#L782) (notifyCallback immediate forward), [src/perf_metrics.h](src/perf_metrics.h) (thresholds)

---

## L. Reference

### Key Classes

#### V1BLEClient

```cpp
class V1BLEClient {
    bool initBLE(bool enableProxy, const char* proxyName);
    bool begin(bool enableProxy, const char* proxyName);
    bool isConnected();
    bool sendCommand(const uint8_t* data, size_t length);
    bool setMute(bool muted);
    bool setMode(uint8_t mode);  // 1=AllBogeys, 2=Logic, 3=AdvLogic
    bool writeUserBytes(const uint8_t* bytes);  // Push profile
    void forwardToProxy(const uint8_t* data, size_t length, uint16_t charUUID);  // Legacy queued path
    void forwardToProxyImmediate(const uint8_t* data, size_t length, uint16_t charUUID);  // Zero-latency path
};
```

**Source:** [src/ble_client.h](src/ble_client.h#L65-L130)

#### PacketParser

```cpp
class PacketParser {
    bool parse(const uint8_t* data, size_t length);
    const DisplayState& getDisplayState() const;
    AlertData getPriorityAlert() const;  // Highest strength alert
    size_t getAlertCount() const;
    bool hasAlerts() const;
};
```

**Source:** [src/packet_parser.h](src/packet_parser.h#L55-L75)

#### V1Display

```cpp
class V1Display {
    bool begin();
    void update(const AlertData& alert, const DisplayState& state, int alertCount);
    void update(const DisplayState& state);  // Resting mode
    void showBootSplash();
    void showScanning();
    void showResting();
    void setBrightness(uint8_t level);
    void drawProfileIndicator(int slot);
    void flush();  // Push canvas to panel
};
```

**Source:** [src/display.h](src/display.h#L28-L60)

#### SettingsManager

```cpp
class SettingsManager {
    void begin();
    const V1Settings& get() const;
    void save();
    void setActiveSlot(int slot);
    AutoPushSlot getSlot(int index) const;
    void setLastV1Address(const char* addr);
};
```

**Source:** [src/settings.h](src/settings.h#L150-L200)

### Data Structures

#### AlertData

```cpp
struct AlertData {
    Band band;           // BAND_NONE, BAND_X, BAND_K, BAND_KA, BAND_LASER
    Direction direction; // DIR_NONE, DIR_FRONT, DIR_SIDE, DIR_REAR
    uint8_t frontStrength;  // 0-6
    uint8_t rearStrength;   // 0-6
    uint32_t frequency;     // MHz (0 for laser)
    bool isValid;
};
```

**Source:** [src/packet_parser.h](src/packet_parser.h#L25-L40)

#### DisplayState

```cpp
struct DisplayState {
    uint8_t activeBands;  // Bitmap of Band enum
    Direction arrows;     // Bitmap of Direction enum
    uint8_t signalBars;   // 0-8 (from V1's LED bitmap)
    bool muted;
    bool systemTest;
    char modeChar;        // 'A', 'L', 'c' for mode indicator
    bool hasMode;
};
```

**Source:** [src/packet_parser.h](src/packet_parser.h#L42-L55)

#### V1UserSettings

6-byte structure mapping to V1 Gen2 user configuration:

| Byte | Bits | Settings |
|------|------|----------|
| 0 | 0-3 | X/K/Ka/Laser enable |
| 0 | 4-7 | Mute behavior, bogey lock |
| 1 | 0-5 | Euro mode, TMF, laser rear, custom freqs |
| 1 | 6-7 | Ka sensitivity (0-3) |
| 2 | 0-7 | Startup, resting, BSM+, automute, K sens, MRCT |
| 3 | 0-7 | X sens, photo systems (DS3D, Redflex, etc) |
| 4-5 | — | Reserved |

**Source:** [src/v1_profiles.h](src/v1_profiles.h#L10-L90)

---

## M. Testing & Validation

> Status: authoritative
> Last validated against scripts: March 12, 2026

This section is the testing and validation policy authority for the repo.

### Evidence Lanes

| Lane | Script / Workflow | Budget | Purpose |
|------|-------------------|--------|---------|
| PR | `./scripts/ci-test.sh` | <= 8 min local / <= 12 min CI | Fast merge-safety gate |
| Nightly | `./scripts/ci-nightly.sh` | <= 60 min | Replay, sanitizer, expanded mutation, soak |
| Pre-release | `./scripts/ci-pre-release.sh` | <= 90 min | Full evidence + hardware qualification |
| Trend | `.github/workflows/stability-trend.yml` | N/A | Non-blocking stability analytics |

### Code Gate (PR Lane)

```bash
./scripts/ci-test.sh
```

The trusted local/code gate. It runs:

- semantic/unit/integration tests
- the tracked critical mutation catalog
- 4 golden captured-log replay scenarios
- deterministic perf scorer regression tests
- compatibility guards
- docs hygiene checks
- frontend/build verification

### Nightly Gate

```bash
./scripts/ci-nightly.sh
```

Runs the PR gate plus:
- full replay corpus
- sanitizer lane (ASan + UBSan) for parser, replay, lockout, volume_fade
- expanded mutation catalog with tier thresholds
- device soak (if hardware available)

### Pre-release Gate

```bash
./scripts/ci-pre-release.sh
```

Runs the nightly gate plus:
- hardware qualification on the release board
- replay with perf evidence extraction
- validation manifest generation

### Hardware Qualification

```bash
./scripts/hardware/test.sh --all --board-id release --strict
./scripts/hardware/test.sh --all --board-id release
./scripts/qualify_hardware_matrix.sh
```

Single-board or multi-board hardware qualification.

`./test.sh` is the root hardware test entry point and delegates to
`./scripts/hardware/test.sh`. With `--all` it runs this fixed sequence:

1. RAD scenario preflight — verifies BLE parser pipeline is functional.
2. Device tests — `run_device_tests.sh --full` (heap, PSRAM, FreeRTOS, etc.).
3. Core soak — 300s real-firmware telemetry soak with `--profile drive_wifi_ap`
   (builds + flashes firmware and filesystem via `--with-fs`).
4. Display soak — 300s display-preview telemetry soak.
5. Assembly — scores all steps, compares against the previous run, updates
   run history.

Uptime continuity is checked between steps to detect unexpected reboots.

`--strict` treats `PASS_WITH_WARNINGS` as a failing exit. Per-board artifact
isolation stores runs under `<artifact-root>/<board-id>/runs/<timestamp>_<sha>/`.

**Prerequisites:** ESP32-S3 connected over USB, setup AP enabled so `http://192.168.35.5/api/debug/metrics` is reachable.

Artifacts: `.artifacts/hardware/test/<board-id>/runs/<timestamp>_<sha>/`

> **Note:** `qualify_hardware.sh` and `device-test.sh` are deprecated and
> redirect to `./scripts/hardware/test.sh`.

### Authoritative Scoring

Use `tools/score_perf_csv.py` as the only authoritative perf scorer:

```bash
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_ap --session longest-connected
python3 tools/score_perf_csv.py /path/to/perf.csv --profile drive_wifi_off --session 1
```

See [PERF_SLOS.md](PERF_SLOS.md) for the numeric thresholds.

`tools/scorecard.py` remains available as a debug/analysis utility, but it is
not release authority.

### Non-Authoritative Tools

These tools still exist, but they are exploratory/manual only:

- `./scripts/run_real_fw_soak.sh`
- `tools/scorecard.py` — stability trend analysis (nightly only, non-blocking)

> `device-test.sh` and `qualify_hardware.sh` are deprecated and redirect to
> `./scripts/hardware/test.sh`.

### Known Validation Gaps

- Replay fixtures are Phase 1 scaffolds — real captured data needed
- No trusted transition stress gate

### Validation Rules

- Reduced but truthful coverage is better than broad fake coverage.
- If a hardware script is exploratory, document it as exploratory.

---

## Known Limitations / TODOs

Based on code analysis:

1. **Single-touch only (no multi-touch):** The AXS15231B touch controller only detects one finger at a time.
   - Tap sequences work fine (single, double, triple-tap) — these are timed sequential touches
   - Long-press/hold gestures work — duration-based detection
   - What doesn't work: pinch-to-zoom, two-finger swipe, or any gesture requiring simultaneous touch points
   - Evidence: `touch_handler.h` comment: "Single-touch support (hardware limitation)"
   
2. **No OTA updates:** Firmware must be flashed via USB.
   - Evidence: No OTA code present in wifi_manager.cpp

---

## Known Issues / Risks

**Fragile areas requiring care:**

1. **BLE Queue Overflow:** If BLE data arrives faster than display processing, oldest packets dropped.
   - Location: [src/modules/ble/ble_queue_module.h](src/modules/ble/ble_queue_module.h) - `bleDataQueue` 64 slots
   - Impact: Only affects local display; proxy forwarding is unaffected (immediate path)
   - Mitigation: Throttle display updates, process queue quickly

2. **Display SPI Timing:** Cannot call display functions from BLE callbacks.
   - Location: All BLE callbacks in `ble_connection.cpp`
   - Mitigation: Always queue data for main loop processing

3. **Password Obfuscation Not Encryption:** XOR obfuscation is NOT cryptographically secure.
   - Location: [src/settings.cpp](src/settings.cpp#L75) (XOR key), [src/settings_nvs.cpp](src/settings_nvs.cpp#L44-L55) (xorObfuscate)
   - Risk: Anyone with flash dump can recover passwords

4. **NVS Wear:** Frequent saves could wear flash (100k write cycles).
   - Mitigation: Settings only save on explicit user action

5. **Battery Voltage Calibration:** ADC readings may vary by hardware unit.
   - Location: [src/battery_manager.cpp](src/battery_manager.cpp) - `BATTERY_MAX_VOLTAGE = 4.1V`

---


*Document generated from source code analysis. Last verified against v4.0.0-dev.*
