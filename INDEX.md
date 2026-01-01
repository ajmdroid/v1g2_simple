# V1 Simple - Repository Index

Quick navigation guide for the V1 Gen2 Simple Display project.

## 📖 Getting Started
- **[README.md](README.md)** - Project overview, features, and hardware requirements
- **[SETUP.md](SETUP.md)** - Installation and first-time setup guide
- **[BUILD_COMMANDS.md](BUILD_COMMANDS.md)** - Building and deploying the project

## 📋 Development
- **[BUILD_COMMANDS.md](BUILD_COMMANDS.md)** - Build script options and manual steps
- **[build.sh](build.sh)** - Automated build script (recommended)
- **[platformio.ini](platformio.ini)** - PlatformIO configuration

## 📚 Documentation
- **[README.md](README.md)** - Feature overview and recent updates
- **[PROGRESS.md](PROGRESS.md)** - Changelog and development history
- **[TODO.md](TODO.md)** - Feature ideas and future work
- **[CLEANUP_SUMMARY.md](CLEANUP_SUMMARY.md)** - What was cleaned up and why

## 🔧 Hardware Reference
- **[WAVESHARE_349.md](WAVESHARE_349.md)** - Waveshare board pinout and quirks
- **[docs/](docs/)** - Feature-specific documentation

## 💻 Source Code Organization

### Core Application
- **[src/main.cpp](src/main.cpp)** - Main application loop and task setup

### BLE & Connectivity
- **[src/ble_client.cpp](src/ble_client.cpp)** - V1 connection, BLE proxy for JBV1 app
- **[src/packet_parser.cpp](src/packet_parser.cpp)** - Valentine1 protocol parsing

### Display & UI
- **[src/display.cpp](src/display.cpp)** - 640×172 display rendering using Arduino_GFX
- **[src/touch_handler.cpp](src/touch_handler.cpp)** - Touch input (tap-to-mute, profile cycle)

### Features
- **[src/battery_manager.cpp](src/battery_manager.cpp)** - Power management and battery reporting
- **[src/v1_profiles.cpp](src/v1_profiles.cpp)** - Auto-push profile system (3 slots)

### Configuration & Storage
- **[src/settings.cpp](src/settings.cpp)** - NVS-based persistent settings
- **[src/storage_manager.cpp](src/storage_manager.cpp)** - Storage abstraction
- **[src/wifi_manager.cpp](src/wifi_manager.cpp)** - WiFi, web UI, and API endpoints

### Utilities & Diagnostics
- **[src/event_ring.cpp](src/event_ring.cpp)** - Circular event log
- **[src/perf_metrics.cpp](src/perf_metrics.cpp)** - Performance tracking and latency metrics

### Configuration Headers
- **[include/config.h](include/config.h)** - Main application configuration
- **[include/color_themes.h](include/color_themes.h)** - Color palette definitions
- **[include/perf_test_flags.h](include/perf_test_flags.h)** - Performance test configuration
- **[include/display_driver.h](include/display_driver.h)** - Display abstraction layer

## 🎨 Web Interface
- **[interface/](interface/)** - SvelteKit web UI source
- **[interface/src/routes/](interface/src/routes/)** - Web pages (dashboard, settings, profiles, etc.)
- **[data/](data/)** - Compiled web assets (deployed to LittleFS)

## 🧪 Testing & Tools
- **[docs/testing.md](docs/testing.md)** - Testing procedures
- **[docs/PERF_TEST_PROCEDURE.md](docs/PERF_TEST_PROCEDURE.md)** - Performance benchmarking
- **[tools/](tools/)** - Build utilities and helper scripts

## 📁 Directory Structure

```
v1g2_simple/
├── src/                    # C++ source files (main application)
├── include/                # C++ headers
├── interface/              # SvelteKit web UI source
├── data/                   # Web assets (LittleFS deployment)
├── docs/                   # Feature documentation
├── tools/                  # Build helpers
├── scripts/                # PlatformIO utilities
│
├── platformio.ini          # Main build configuration
├── build.sh                # Build automation script
├── README.md               # User guide
├── SETUP.md                # Installation guide
├── BUILD_COMMANDS.md       # Build documentation
├── PROGRESS.md             # Changelog
├── TODO.md                 # Feature backlog
├── CLEANUP_SUMMARY.md      # What was cleaned (this session)
└── WAVESHARE_349.md        # Hardware reference
```

## 🚀 Quick Start

```bash
# Clone and open in VS Code
git clone https://github.com/ajmdroid/v1g2_simple
cd v1g2_simple
code .

# Install PlatformIO extension in VS Code

# Build everything and upload
./build.sh --all

# Or build firmware only
./build.sh

# For development with live feedback
./build.sh --skip-web --upload --monitor
```

## 🔍 Key Features

| Feature | Location | Notes |
|---------|----------|-------|
| BLE Connection | `ble_client.cpp` | V1 Gen2 connection + JBV1 proxy |
| Display Output | `display.cpp` | 640×172 AMOLED with bands, signal, direction |
| Touch Control | `touch_handler.cpp` | Tap-to-mute, profile cycle |
| Profiles | `v1_profiles.cpp` | Auto-push 3 profile slots |
| Web UI | `interface/` | WiFi setup, configuration, profile management |
| Performance Metrics | `perf_metrics.cpp` | Latency tracking and diagnostics |
| Settings | `settings.cpp` | Persistent configuration via NVS |

## 📊 Build Status

- **Firmware size:** ~1.7 MB / 6.5 MB available
- **RAM usage:** ~17% / 320 KB available
- **Build time:** ~26 seconds (clean build)
- **Platform:** PlatformIO + Arduino framework

---

**Last Updated:** January 1, 2026  
**Status:** ✅ Production Ready  
**Stability:** Stable with active development
