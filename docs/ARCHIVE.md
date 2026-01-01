# V1G2 Simple - Documentation Archive

This document consolidates historical documentation, development notes, and reference material from various markdown files that were previously scattered across the repository. The information here is preserved for reference but may not be actively maintained.

**Last Updated**: January 1, 2026

---

## Table of Contents

1. [Build Commands Reference](#build-commands-reference)
2. [Hardware Reference (Waveshare 3.49")](#hardware-reference)
3. [BLE Reliability & Proxy Fixes](#ble-reliability--proxy-fixes)
4. [WiFi & Web UI Implementation](#wifi--web-ui-implementation)
5. [Performance Testing](#performance-testing)
6. [Replay Mode (UI Testing)](#replay-mode)
7. [Repository Structure](#repository-structure)
8. [GitHub Actions CI/CD](#github-actions-cicd)

---

## Build Commands Reference

### Quick Start - Build Script

```bash
# Build everything and upload (recommended)
./build.sh --all

# Just build (no upload)
./build.sh

# Clean build and upload everything
./build.sh --clean --all

# Build firmware only, skip web (faster)
./build.sh --skip-web --upload

# Show all options
./build.sh --help
```

### Build Script Options

| Option | Description |
|--------|-------------|
| `--clean` or `-c` | Clean build artifacts first |
| `--upload` or `-u` | Upload firmware after build |
| `--upload-fs` or `-f` | Upload filesystem (web interface) |
| `--monitor` or `-m` | Open serial monitor after upload |
| `--all` or `-a` | Do everything (upload fs + firmware + monitor) |
| `--skip-web` or `-s` | Skip web interface build |

### Manual Build Commands

```bash
# Build firmware
pio run -e waveshare-349

# Upload firmware
pio run -e waveshare-349 -t upload

# Upload filesystem (web UI)
pio run -e waveshare-349 -t uploadfs

# Serial monitor
pio device monitor

# Complete rebuild
cd interface && npm run deploy && cd .. && \
pio run -t clean && pio run && \
pio run -t uploadfs && pio run -t upload
```

### Web Interface Development

```bash
cd interface
npm install          # First time setup
npm run dev          # Development server (hot reload)
npm run build        # Production build
npm run deploy       # Build + compress + copy to data/
```

---

## Hardware Reference

### Waveshare ESP32-S3-Touch-LCD-3.49 Pinout

| Function | Pins |
|----------|------|
| Display (QSPI) | CS=9, SCLK=10, DATA0=11, DATA1=12, DATA2=13, DATA3=14, RST=21, BL=8 |
| Touch (I2C) | SDA=17, SCL=18 @ 400kHz, Address=0x3B |
| SD Card (SDMMC 1-bit) | CLK=41, CMD=39, D0=40 |

### Hardware Quirks

- **Backlight PWM is inverted**: `0` = full brightness, `255` = off
- **Display orientation**: 172×640 canvas rotated to 640×172 landscape
- **Touch**: Tap-only; gesture support not exposed by AXS15231B driver

### Troubleshooting

- **Backlight dark?** Write `0` to `LCD_BL` (PWM inverted)
- **Touch not working?** Check I2C at `0x3B` on SDA=17/SCL=18
- **SD not mounting?** Ensure SDMMC pins are free, use 1-bit mode

---

## BLE Reliability & Proxy Fixes

### Key Issues Addressed

1. **EBUSY errors (error 13)**: BLE_HS_EBUSY during connection attempts
2. **Overlapping operations**: Scan/connect/advertising state conflicts
3. **Proxy disconnects**: Reason 531 (0x13) - remote terminated

### Critical Fixes

#### 1. Kenny's Init Pattern (NimBLE 2.x)
```cpp
// Correct order for dual-role BLE
NimBLEDevice::init("V1 Proxy");
NimBLEDevice::setDeviceName(proxyName);
NimBLEDevice::setPower(ESP_PWR_LVL_P9);
// NO setOwnAddrType() or setMTU() for proxy mode
// NO security settings (apps expect open connections)
```

#### 2. Server Callbacks After Service Start
```cpp
pProxyService->start();
// THEN set callbacks (critical ordering)
pServer->setCallbacks(pProxyServerCallbacks);
```

#### 3. Full V1 API (6 Characteristics)
| UUID | Type | Purpose |
|------|------|---------|
| 92A0B2CE | Notify | Display data SHORT (primary alerts) |
| 92A0B4E0 | Notify | V1 out LONG |
| 92A0B6D4 | WriteNR | Client write SHORT (commands) |
| 92A0B8D2 | WriteNR | Client write LONG |
| 92A0BCE0 | Notify | Additional notify |
| 92A0BAD4 | Write+WriteNR | Alternate write |

#### 4. Scan Stop Settle Time
```cpp
static constexpr unsigned long SCAN_STOP_SETTLE_MS = 500;
```

#### 5. Process Proxy Queue in Main Loop
```cpp
// In main.cpp loop - CRITICAL for data flow
bleClient.processProxyQueue();
```

### Connection State Machine

```
DISCONNECTED → SCANNING → SCAN_STOPPING → CONNECTING → CONNECTED
                                    ↓
                                BACKOFF (on failure)
```

---

## WiFi & Web UI Implementation

### WiFi Control

```bash
# Serial commands
wifi on      # Enable WiFi and reboot
wifi off     # Disable WiFi and reboot
wifi status  # Show current state
```

### API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | System status JSON |
| `/api/profile/push` | POST | Queue profile push |
| `/api/wifi/off` | POST | Disable WiFi, reboot |
| `/api/debug/metrics` | GET | Performance counters |
| `/api/debug/events` | GET | Event ring buffer |

### Status JSON Response

```json
{
  "ble_state": "CONNECTED",
  "ble_connected": true,
  "proxy_connected": true,
  "heap_free": 245000,
  "wifi_enabled": true,
  "uptime_sec": 3600
}
```

### Failsafe UI

Access at `http://192.168.4.1/failsafe` when in Setup Mode.
- Self-contained HTML (no external dependencies)
- Auto-refresh every 3 seconds
- Profile push, WiFi disable actions

---

## Performance Testing

### Perf Test Flags (`include/perf_test_flags.h`)

| Flag | Effect |
|------|--------|
| `PERF_TEST_DISABLE_WIFI` | Skip WiFi processing |
| `PERF_TEST_DISABLE_TOUCH` | Skip touch polling |
| `PERF_TEST_DISABLE_BATTERY` | Skip battery reads |
| `PERF_TEST_EARLY_DRAIN` | Move BLE processing earlier |
| `PERF_TEST_DISABLE_PROXY` | Skip proxy forwarding |
| `PERF_TEST_DISABLE_THROTTLE` | Always draw (no throttle) |

### Test Procedure

1. Edit `include/perf_test_flags.h` - uncomment ONE flag
2. Build: `pio run`
3. Flash: `pio run -t upload`
4. Monitor for 2 minutes with active alerts
5. Record `PERF TEST RESULTS` output

### Performance Counters

| Counter | Description |
|---------|-------------|
| `rxPackets` | BLE notifications received |
| `queueDrops` | Packets dropped (queue full) |
| `parseSuccesses` | Successfully parsed packets |
| `displayUpdates` | Frames drawn |
| `displaySkips` | Updates skipped (throttled) |

---

## Replay Mode

Enable packet replay for UI testing without BLE hardware.

### Enable

```cpp
// In include/config.h
#define REPLAY_MODE
```

### Features

- Disables BLE stack entirely
- Injects simulated V1 packets
- Loops through alert sequence (~11 seconds)
- Exercises display, parsing, alert handling

### Memory Savings

- RAM: -5.9KB (18.6% → 16.8%)
- Flash: -183KB (32.7% → 29.9%)

### Packet Sequence

1. Clear (2s) → Ka alert → Muted display (1s)
2. Clear → X band → Clear
3. K band rear → Clear → Laser → Clear (loop)

---

## Repository Structure

```
v1g2_simple/
├── src/                    # C++ source files
│   ├── main.cpp            # Main loop, auto-push, touch
│   ├── ble_client.cpp      # V1 BLE + JBV1 proxy
│   ├── display.cpp         # 640×172 AMOLED rendering
│   ├── packet_parser.cpp   # V1 protocol parsing
│   ├── wifi_manager.cpp    # WiFi, web server, APIs
│   ├── settings.cpp        # NVS persistence
│   ├── v1_profiles.cpp     # Profile system
│   └── ...
├── include/                # C++ headers
│   ├── config.h            # Main configuration
│   ├── color_themes.h      # Color palettes
│   └── ...
├── interface/              # SvelteKit web UI
├── data/                   # LittleFS web assets
├── docs/                   # Documentation
├── tools/                  # Build utilities
├── platformio.ini          # Build config
├── build.sh                # Build automation
├── README.md               # User guide
└── SETUP.md                # Installation guide
```

---

## GitHub Actions CI/CD

### Manual Build

1. Go to repo on GitHub → **Actions** tab
2. Click **Build Firmware & Web Interface**
3. Click **Run workflow**
4. Download artifacts when complete

### Auto-Triggers

Workflow runs on:
- Push to `main` or `master`
- Pull requests
- Changes to `src/**`, `include/**`, `interface/**`, `platformio.ini`

### Build Artifacts

| File | Description | Flash Address |
|------|-------------|---------------|
| `firmware.bin` | Main firmware | 0x10000 |
| `bootloader.bin` | ESP32-S3 bootloader | 0x0000 |
| `partitions.bin` | Partition table | 0x8000 |
| `littlefs.bin` | Web interface filesystem | 0x3D0000 |

Artifacts retained for 30 days.

---

## Source Files Archived From

This document was compiled from:
- `BUILD_COMMANDS.md` - Build script documentation
- `WAVESHARE_349.md` - Hardware reference
- `INDEX.md` - Repository navigation
- `CLEANUP_SUMMARY.md` - Cleanup history
- `docs/BLE_RELIABILITY_FIXES.md` - BLE debugging
- `docs/BUILD.md` - Build commands
- `docs/PERF_TEST_PROCEDURE.md` - Performance testing
- `docs/REPLAY_MODE.md` - UI testing mode
- `docs/WIFI_FAILSAFE_UI.md` - WiFi implementation
- `docs/testing.md` - Testing infrastructure
- `.github/workflows/README.md` - CI/CD documentation
- `.github/pull_request_template.md` - PR template

---

*End of Archive*
