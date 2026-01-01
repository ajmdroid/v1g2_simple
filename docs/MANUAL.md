# V1 Gen2 Simple Display - Technical Manual

**Version:** 1.0.0  
**Hardware:** Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640×172 AMOLED)  
**Generated from source analysis:** January 2026

---

## Table of Contents

1. [Quick Start](#a-quick-start)
2. [Overview](#b-overview)
3. [System Architecture](#c-system-architecture)
4. [Boot Flow](#d-boot-flow)
5. [Bluetooth / Protocol](#e-bluetooth--protocol)
6. [Display / UI](#f-display--ui)
7. [Storage](#g-storage)
8. [Configuration & Build Options](#h-configuration--build-options)
9. [Troubleshooting](#i-troubleshooting)
10. [Developer Guide](#j-developer-guide)
11. [Reference](#k-reference)

---

## A. Quick Start

### Build & Flash

```bash
# 1. Clone repository
git clone <repo-url>
cd v1g2_simple

# 2. Build web interface (requires Node.js)
cd interface
npm install
npm run build
npm run deploy  # Copies to data/
cd ..

# 3. Build firmware
pio run -e waveshare-349

# 4. Flash filesystem (web UI)
pio run -t uploadfs -e waveshare-349

# 5. Flash firmware
pio run -t upload -e waveshare-349

# 6. Monitor serial output
pio device monitor -b 115200
```

### Verify Installation

1. Device shows boot splash (if power-on reset)
2. Screen displays "SCAN" with resting animation
3. Connect to WiFi AP: `V1-Simple` / password: `setupv1g2`
4. Browse to `http://192.168.35.5`
5. Web UI should load (SvelteKit) or fallback dashboard

**Source:** [platformio.ini](platformio.ini#L1-L50), [build.sh](build.sh#L1-L30), [interface/scripts/deploy.js](interface/scripts/deploy.js#L1-L30)

---

## B. Overview

### What This Project Does

A touchscreen remote display for the Valentine One Gen2 radar detector. Connects via Bluetooth Low Energy (BLE) to show:

- **Radar alerts:** Band (X, K, Ka, Laser), signal strength (0-6 bars), direction (front/side/rear)
- **Frequency display:** 7-segment style showing detected frequency in GHz
- **Mute control:** Tap screen to mute/unmute active alerts
- **Profile management:** 3-slot auto-push system for V1 settings profiles
- **BLE proxy:** Allows JBV1/V1 Companion apps to connect through this device

### Supported Hardware

| Component | Specification |
|-----------|---------------|
| Board | Waveshare ESP32-S3-Touch-LCD-3.49 |
| CPU | ESP32-S3 @ 240MHz, 16MB Flash, 8MB PSRAM |
| Display | AXS15231B QSPI AMOLED, 640×172 pixels |
| Touch | Integrated AXS15231B capacitive touch |
| Storage | LittleFS (internal), SD card (optional) |
| Battery | Optional LiPo via TCA9554 power management |

**Source:** [platformio.ini](platformio.ini#L1-L50), [include/config.h](include/config.h#L1-L20), [src/battery_manager.h](src/battery_manager.h#L1-L30)

### Key Features

1. **BLE Client:** Connects to V1 Gen2 (device names starting with "V1G" or "V1-")
2. **BLE Server (Proxy):** Advertises as "V1C-LE-S3" for JBV1 compatibility
3. **Tap-to-Mute:** Single/double tap during alert toggles mute
4. **Triple-Tap Profile Cycle:** Switch between 3 auto-push slots when idle
5. **Web Configuration:** AP mode at 192.168.35.5 for settings
6. **4 Color Themes:** Standard, High Contrast, Stealth, Business

**Source:** [src/main.cpp](src/main.cpp#L1-L25), [src/ble_client.cpp](src/ble_client.cpp#L1-L20)

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

| File | Lines | Purpose |
|------|-------|---------|
| `main.cpp` | 1135 | Application entry, loop, touch handling |
| `ble_client.cpp` | 1699 | NimBLE client/server, V1 connection |
| `display.cpp` | 1605 | Arduino_GFX drawing, 7/14-segment digits |
| `wifi_manager.cpp` | 1629 | WebServer, API endpoints, LittleFS serving |
| `packet_parser.cpp` | 287 | ESP packet framing and decoding |
| `settings.cpp` | 625 | Preferences (NVS) storage |
| `v1_profiles.cpp` | ~300 | Profile JSON on SD/LittleFS |
| `battery_manager.cpp` | ~500 | ADC, TCA9554 I/O expander |
| `storage_manager.cpp` | ~100 | SD/LittleFS mount abstraction |
| `touch_handler.cpp` | ~100 | AXS15231B I2C touch polling |
| `event_ring.cpp` | ~120 | Debug event logging |
| `perf_metrics.cpp` | ~100 | Latency tracking |

### Data Flow

```
V1 Gen2 (BLE)
     │
     │ Notify (B2CE characteristic)
     ▼
┌─────────────┐    Queue (64 slots)    ┌─────────────┐
│ onV1Data()  │ ──────────────────────▶│processBLE() │
│ (BLE task)  │    BLEDataPacket       │(main loop)  │
└─────────────┘                        └──────┬──────┘
                                              │
                                   ┌──────────┴──────────┐
                                   │                     │
                              ┌────▼────┐          ┌─────▼─────┐
                              │ Parser  │          │ Proxy Fwd │
                              │(framing)│          │ (JBV1)    │
                              └────┬────┘          └───────────┘
                                   │
                              ┌────▼────┐
                              │ Display │
                              │ update  │
                              └─────────┘
```

**Source:** [src/main.cpp](src/main.cpp#L48-L55) (queue definition), [src/main.cpp](src/main.cpp#L195-L210) (onV1Data callback), [src/main.cpp](src/main.cpp#L467-L540) (processBLEData)

### Threading Model

| Context | Description | Critical Operations |
|---------|-------------|---------------------|
| **Main loop** | Arduino `loop()` at ~200Hz | Display SPI, touch I2C, WiFi |
| **BLE task** | NimBLE internal task | Notifications, connection events |
| **FreeRTOS queue** | `bleDataQueue` (64 × 260 bytes) | Decouples BLE callbacks from SPI |

**Key constraint:** SPI operations (display) must NOT occur in BLE callbacks.

**Source:** [src/main.cpp](src/main.cpp#L195-L210) (callback queues data), [src/main.cpp](src/main.cpp#L467-L475) (main loop processes)

### Timing Constraints

| Operation | Timing | Source |
|-----------|--------|--------|
| Display draw minimum interval | 20ms (~50fps max) | `DISPLAY_DRAW_MIN_MS` in main.cpp:58 |
| Display update check | 100ms | `DISPLAY_UPDATE_MS` in config.h:62 |
| Status serial print | 1000ms | `STATUS_UPDATE_MS` in config.h:63 |
| Touch debounce | 200ms | touch_handler.cpp |
| Tap window (triple-tap) | 600ms | `TAP_WINDOW_MS` in main.cpp:167 |
| Local mute timeout | 2000ms | `LOCAL_MUTE_TIMEOUT_MS` in main.cpp:155 |

---

## D. Boot Flow

### Initialization Sequence

```
1. delay(100)                          // USB stabilize
2. Create bleDataQueue (64 slots)      // FreeRTOS queue
3. Serial.begin(115200)
4. batteryManager.begin()              // CRITICAL: Latch power early
5. display.begin()                     // QSPI init, canvas allocation
6. Check esp_reset_reason()            // Decide splash vs skip
7. IF power-on: showBootSplash(1500ms)
8. showScanning()                      // "SCAN" text
9. settingsManager.begin()             // Load from NVS
10. storageManager.begin()             // Mount SD or LittleFS
11. v1ProfileManager.begin()           // Profile filesystem access
12. wifiManager.begin()                // Start AP, web server
13. touchHandler.begin()               // I2C touch init
14. bleClient.initBLE()                // NimBLE stack
15. bleClient.begin()                  // Start scanning
16. loop()                             // Main application loop
```

**Source:** [src/main.cpp](src/main.cpp#L745-L920) (setup function)

### Boot Splash Logic

- **Power-on reset (`ESP_RST_POWERON`):** Show 640×172 logo for 1500ms
- **Software reset / upload:** Skip splash for faster iteration
- **Crash restart:** Skip splash

**Source:** [src/main.cpp](src/main.cpp#L810-L820), [src/display.cpp](src/display.cpp) showBootSplash()

### Fallback Behavior

| Failure | Behavior |
|---------|----------|
| Display init fails | Serial error, infinite delay loop |
| BLE init fails | Serial error, showDisconnected(), infinite loop |
| Touch init fails | Warning logged, continues without touch |
| Storage init fails | Warning logged, profiles disabled |
| WiFi/AP fails | Warning logged, continues without web UI |

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
│DISCONNECTED │◀────────────────────────────┐
└──────┬──────┘                             │
       │ startScanning()                    │ onDisconnect()
       ▼                                    │
┌─────────────┐                             │
│  SCANNING   │                             │
└──────┬──────┘                             │
       │ V1 found (name starts "V1G"/"V1-") │
       ▼                                    │
┌─────────────┐                             │
│SCAN_STOPPING│ (300ms settle)              │
└──────┬──────┘                             │
       │ scan stopped                       │
       ▼                                    │
┌─────────────┐   fail (3 attempts)   ┌─────┴─────┐
│ CONNECTING  │──────────────────────▶│  BACKOFF  │
└──────┬──────┘                       └───────────┘
       │ success                       (exponential)
       ▼
┌─────────────┐
│  CONNECTED  │
└─────────────┘
```

**Source:** [src/ble_client.h](src/ble_client.h#L30-L50) (BLEState enum), [src/ble_client.cpp](src/ble_client.cpp#L350-L450) (scan callbacks), [src/ble_client.cpp](src/ble_client.cpp#L550-L650) (connectToServer)

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

V1 Gen2 sends raw RSSI values. Mapped to 0-6 bars using threshold tables:

```cpp
// Ka thresholds: 0x00, 0x8F, 0x99, 0xA4, 0xAF, 0xB5, 0xFF
// K thresholds:  0x00, 0x87, 0x95, 0xA3, 0xB1, 0xBF, 0xFF
// X thresholds:  0x00, 0x95, 0xA5, 0xB3, 0xC0, 0xCC, 0xFF
```

**Source:** [src/packet_parser.cpp](src/packet_parser.cpp#L140-L165)

### Queue / Buffering

- **Queue:** 64-slot FreeRTOS queue, each slot 260 bytes
- **Overflow handling:** Drop oldest packet if full
- **Buffer accumulation:** `rxBuffer` accumulates chunks until 0xAA...0xAB frame complete
- **Max buffer size:** 512 bytes, trimmed if exceeded

**Source:** [src/main.cpp](src/main.cpp#L48-L55), [src/main.cpp](src/main.cpp#L195-L210), [src/main.cpp](src/main.cpp#L495-L510)

### Proxy Mode (JBV1 Compatibility)

When `proxyBLE=true`:
1. Device advertises as "V1C-LE-S3" after V1 connects
2. JBV1/V1 Companion can connect as secondary client
3. All V1 notifications forwarded via `forwardToProxy()`
4. Commands from JBV1 forwarded to V1

**Source:** [src/ble_client.cpp](src/ble_client.cpp#L260-L280) (initProxyServer), [src/ble_client.h](src/ble_client.h#L150-L160)

### Connection Parameters

```cpp
// NimBLE connection params: min/max interval, latency, timeout
pClient->setConnectionParams(40, 80, 0, 400);  // 50-100ms, 0 latency, 4s timeout
pClient->setConnectTimeout(10);  // 10 second connect timeout
```

**Source:** [src/ble_client.cpp](src/ble_client.cpp#L560-L565)

### Backoff on Failure

- Base: 500ms
- Max: 10000ms
- Formula: `BACKOFF_BASE_MS * (1 << min(failures-1, 4))`
- Hard reset after 5 consecutive failures

**Source:** [src/ble_client.cpp](src/ble_client.cpp#L610-L630)

---

## F. Display / UI

### Screen Layout (640×172 Landscape)

```
┌──────────────────────────────────────────────────────────────────────┐
│ [WiFi]  [Profile]                                    [Battery]       │ Status bar (~20px)
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│    [1]     35.500      ◀▶       ██████                              │ Main area
│   [Ka]                          ██████                              │ - Bogey count
│  (band)    (7-seg freq)  (arrows)  (signal bars)                    │ - Frequency
│                                                                      │ - Direction
│                                                                      │ - Strength
└──────────────────────────────────────────────────────────────────────┘
```

### Display States

| State | Trigger | Appearance |
|-------|---------|------------|
| Boot splash | Power-on reset | Full-screen logo image |
| Scanning | Not connected | "SCAN" in 7-segment |
| Resting | Connected, no alerts | Dim gray segments |
| Alert | Alert data received | Band color, frequency, bars |
| Muted | Muted alert | Gray color override |
| Low battery | Critical voltage | Warning screen |
| Shutdown | Power off | "Goodbye" message |

**Source:** [src/display.h](src/display.h#L40-L55), [src/display.cpp](src/display.cpp) various show*() methods

### Color Themes

| Theme | Background | Ka | K | X | Use Case |
|-------|------------|----|----|---|----------|
| Standard | Black | Red | Blue | Green | Day driving |
| High Contrast | Black | Bright Red | Bright Blue | Bright Green | Bright sun |
| Stealth | Black | Dark Red | Dark Blue | Dark Green | Night driving |
| Business | Navy | Amber | Steel | Teal | Professional |

**Source:** [include/color_themes.h](include/color_themes.h#L1-L120)

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

**Source:** [src/display.cpp](src/display.cpp#L160-L200)

### 14-Segment Letters

For band labels (Ka, K, X, LASER):

```cpp
// Segment bit definitions for A-Z characters
// Supports: A, C, D, E, F, G, H, I, J, K, L, M, N, O, P, R, S, T, U, V, X, Y
```

**Source:** [src/display.cpp](src/display.cpp#L180-L210)

### Refresh Model

1. **Throttled:** Minimum 20ms between draws (`DISPLAY_DRAW_MIN_MS`)
2. **Canvas-buffered:** Draw to `Arduino_Canvas`, then `flush()` to panel
3. **Partial update:** Only full-screen redraws (no dirty rectangle tracking)

**Performance path:**
- BLE notify → queue → parse → update() → flush()
- Target: <100ms end-to-end latency

**Source:** [src/main.cpp](src/main.cpp#L635-L640) (throttle check), [src/display.cpp](src/display.cpp) flush()

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
| apSSID | String | "V1-Display" | AP network name |
| apPassword | String | "valentine1" (obfuscated) | AP password |
| proxyBLE | bool | true | BLE proxy enabled |
| proxyName | String | "V1C-LE-S3" | Proxy advertised name |
| brightness | uint8 | 200 | Display brightness 0-255 |
| colorTheme | int | 0 | Theme enum |
| autoPush | bool | false | Auto-push on connect |
| activeSlot | int | 0 | Active profile slot 0-2 |
| slot0prof | String | "" | Slot 0 profile name |
| slot0mode | int | 0 | Slot 0 V1 mode |
| lastV1Addr | String | "" | Last connected V1 address |

**Source:** [src/settings.cpp](src/settings.cpp#L50-L180)

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

**Source:** [src/settings.cpp](src/settings.cpp#L1-L40)

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

**Source:** [src/settings.cpp](src/settings.cpp#L250-L280) (backupToSD, restoreFromSD)

---

## H. Configuration & Build Options

### PlatformIO Environment

Single environment: `waveshare-349`

```ini
[env:waveshare-349]
platform = espressif32
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

board_build.partitions = default_16MB.csv
board_build.filesystem = littlefs
```

**Source:** [platformio.ini](platformio.ini#L1-L50)

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

**Source:** [include/config.h](include/config.h#L65-L68), [src/perf_metrics.h](src/perf_metrics.h#L25-L55)

### Runtime Configuration

All settings modifiable via web UI at `http://192.168.35.5`:

- **Settings page:** WiFi, BLE proxy, brightness, theme
- **Colors page:** Custom per-element colors
- **Profiles page:** Create/edit/delete V1 profiles
- **Auto-push page:** Configure 3 profile slots
- **Devices page:** Manage known V1 addresses

### Replay Mode

For UI development without V1 hardware:

1. Uncomment `#define REPLAY_MODE` in [include/config.h](include/config.h#L68)
2. Rebuild and flash
3. Device cycles through sample alert packets

**Source:** [src/main.cpp](src/main.cpp#L355-L450) (replay packet definitions)

---

## I. Troubleshooting

### Serial Debug Output

Connect at 115200 baud. Key prefixes:

| Prefix | Source |
|--------|--------|
| `[Battery]` | battery_manager.cpp |
| `[BLE_SM]` | ble_client.cpp state machine |
| `[SetupMode]` | wifi_manager.cpp |
| `[Settings]` | settings.cpp |
| `[AutoPush]` | main.cpp auto-push state machine |
| `[Touch]` | touch_handler.cpp |

### Common Issues

| Symptom | Cause | Solution |
|---------|-------|----------|
| "SCAN" never changes | V1 not advertising | Ensure V1 Gen2 BLE enabled |
| Connects then disconnects | Bad bonding info | V1 will auto-clear; wait 10s |
| No web UI | LittleFS empty | Run `pio run -t uploadfs` |
| Touch not working | I2C init failed | Check serial for "[Touch] ERROR" |
| Display blank | QSPI init failed | Check pin definitions |
| Battery icon missing | USB power detected | Normal - only shows on battery |

### BLE Connection Debugging

Enable verbose logging by checking serial output for:

```
[BLE_SM] SCANNING -> SCAN_STOPPING | Reason: V1 found
[BLE_SM] Connect attempt 1/3
[BLE_SM] Service discovery completed
[BLE_SM] Subscribed to display data notifications
```

### Web API Endpoints

Test connectivity:
- `GET /api/status` - JSON with connection state
- `GET /api/settings` - Current settings
- `GET /api/profiles` - List profiles
- `GET /api/debug/events` - Event ring dump

### Event Ring

Debug events viewable at `/api/debug/events`:

```json
{
  "events": [
    {"t": 12345, "type": "EVT_BLE_CONNECT", "data": 0},
    {"t": 12400, "type": "EVT_PARSE_OK", "data": 49}
  ]
}
```

**Source:** [src/event_ring.h](src/event_ring.h#L1-L100)

---

## J. Developer Guide

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
3. **Web API:** Add endpoint in `wifi_manager.cpp` if needed
4. **Settings:** Add to `settings.h`/`settings.cpp` if persistent
5. **Web UI:** Add page in `interface/src/routes/`

### Testing

No automated tests exist. Manual testing procedure:

1. **Build test:** `pio run -e waveshare-349`
2. **Static analysis:** `./scripts/pio-check.sh` (clang-tidy)
3. **Size check:** `./scripts/pio-size.sh`
4. **Functional test:**
   - Power on, verify splash
   - Connect to V1, verify alerts display
   - Test tap-to-mute
   - Test triple-tap profile switch
   - Test web UI pages

### Performance-Sensitive Paths

**⚠️ Hot paths - avoid blocking:**

1. `onV1Data()` - BLE callback, must queue quickly
2. `processBLEData()` - Main loop, target <10ms
3. `display.update()` + `flush()` - Target <15ms total
4. `forwardToProxy()` - Proxy forwarding, must not block

**Source:** [src/main.cpp](src/main.cpp#L195-L210) (onV1Data), [src/perf_metrics.h](src/perf_metrics.h#L95-L100) (thresholds)

---

## K. Reference

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
    void forwardToProxy(const uint8_t* data, size_t length, uint16_t charUUID);
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
    uint8_t signalBars;   // 0-6
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

## Known Limitations / TODOs

Based on code analysis:

1. **No STA mode:** WiFi is AP-only (192.168.35.5). STA code exists but is disabled.
   - Evidence: `wifi_manager.cpp` only calls `startSetupMode()`, no `connectSTA()`
   
2. **No alert logging:** SQLite/alert history code was removed.
   - Evidence: Comment in `platformio.ini`: "Sqlite3Esp32 removed - alert logging has been fully removed"
   
3. **Single-touch only:** Hardware limitation of AXS15231B touch controller.
   - Evidence: `touch_handler.h` comment: "Single-touch support (hardware limitation)"
   
4. **No OTA updates:** Firmware must be flashed via USB.
   - Evidence: No OTA code present in wifi_manager.cpp
   
5. **Profile verification disabled:** Write-and-verify was causing issues.
   - Evidence: `ble_client.h:109` comment about verification

---

*Document generated from source code analysis. All file references point to actual source locations.*
