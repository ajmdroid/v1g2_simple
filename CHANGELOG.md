# Changelog

All notable changes to the V1-Simple project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [4.0.0-dev] - 2026-02-25

> Note: no tagged releases were published between `3.0.7` and `4.0.0-dev`;
> ongoing work during that gap landed directly in the dev cycle summarized here.

### Added

**Module Extraction (625 commits since 2026-02-15)**
- Extracted 15 module directories under `src/modules/` (139 files, ~17,500 lines).
- Loop phase orchestration extracted to `main_loop_phases.cpp` (~190 lines) with 10 phase-router modules (`LoopIngestModule`, `LoopConnectionEarlyModule`, `LoopDisplayModule`, etc.).
- Core service splits: `ble_runtime.cpp` (511 lines), `packet_parser_alerts.cpp` (582 lines), `settings_restore.cpp` (782 lines).
- Boot-time helpers extracted to `main_boot.cpp` (248 lines).
- Lockout/learner save state machines extracted to `main_persist.cpp` (445 lines).
- WiFi subsystem modularized into dedicated runtime, policy, cadence, and visual-sync modules.
- BLE connection runtime and state dispatch modules extracted with Providers DI pattern.
- Speed-volume runtime, speaker-quiet sync, and voice-speed sync modules added.
- `SystemEventBus` and `PeriodicMaintenanceModule` for cross-cutting concerns.
- `DebugPerfFilesService` extracted from debug API for perf-file management.
- 8 CI contract scripts enforcing architectural invariants.

**Quality + Runtime Hardening**
- Expanded to 76 native test suites, 960 test cases (`pio test -e native`).
- Drive-scenario integration tests (15 scenarios).
- Lockout runtime stack fully integrated (capture, learner, enforcer, store/index, zone APIs).
- Heap safety hardened with RAII ownership and teardown guards.

- **Security Warning**: Default password warning banner in web UI
  - Shows on all pages when using factory default password
  - Dismissible per session
  - Links to Settings page for easy password change

**WiFi Client Mode**
- Connect to external WiFi networks for internet access
- Maintains AP mode for device access while connected

**Performance**
- Perf CSV schema expanded with subsystem timing (GPS, lockout).
- Audio play/busy/fail counters, signal-observation queue drop counters wired.
- Perf CSV SLO scorecard tooling added.

### Changed
- **Architecture**: Main-loop `loop()` reduced to thin orchestrator (~80 lines); `configure*Module()` wiring functions handle DI (~290 lines).
- **DI Patterns**: Three new dependency-injection patterns documented (Providers, Callbacks hybrid, Pass-by-ref).
- **Display**: Per-element `s_force*Redraw` statics replaced by shared `DisplayDirtyFlags` struct with `dirty.setAll()` invalidation.
- **Boot**: BLE initialization reordered before storage; WiFi deferred; settle time absorbed.
- **CI/CD**: Build runs unit tests before firmware compilation; firmware size budget, static analysis, and interface lint gates added.
- **Input Validation**: Added proxy_name length limit (32 chars).
- **API**: Added `isDefaultPassword` flag to `/api/settings` response.
- **Docs**: Full documentation audit — ARCHITECTURE.md, MANUAL.md, API.md aligned to code.

### Fixed
- WiFi STA config recovery when NVS keys are missing (SD secret fallback).
- Display flush contract stabilized against line-offset drift.
- Dirty lockout zones and learner candidates flushed on shutdown.

### Security
- Default password warning encourages users to change factory credentials

---

## [3.0.7] - Previous Release

### Features
- Full V1 BLE connectivity with packet parsing
- Auto-lockout with GPS geofencing
- V1 profile management with auto-push
- Custom display themes and colors
- SD card backup functionality
- Runtime diagnostics and perf counters

### Technical
- ESP32-S3 on Waveshare 3.49" display
- BLE connection (V1)
- FreeRTOS multi-tasking
- LittleFS + SD card storage
- Responsive SvelteKit web interface

---

## Version History

| Version | Date | Highlights |
|---------|------|------------|
| 4.0.0-dev | 2026-02-25 | Modular architecture, 141 module files, 960 tests, CI contracts |
| 3.0.7 | 2026 | Quality baseline before 4.x refactors |
| 3.0.x | 2024 | Auto-lockout improvements |
| 2.x.x | 2024 | Auto-lockout, profiles |
| 1.x.x | 2023 | Initial release, basic V1 display |

---

## Upgrade Notes

### From 3.x to 4.0.0-dev

**Pre-release build.** API/behavior changes exist versus 3.0.7; validate integrations before production use.

Recommended post-upgrade actions:
1. Change default WiFi password if not already done
2. Review new security warning banner
3. Re-validate any tooling/scripts that call REST endpoints against `docs/API.md`

### From 2.x.x to 3.x.x

**Settings migration required.**

1. Backup settings via web UI before upgrade
2. Perform firmware upgrade
3. Restore settings backup
4. Re-configure any missing options

---

## Development

### Running Tests
```bash
# Run all unit tests
pio test -e native

# Run specific test
pio test -e native -f test_packet_parser
```

### Building Firmware
```bash
# Build + flash firmware/filesystem
./build.sh --all

# Build firmware only
pio run -e waveshare-349
```

### CI Status
- Tests must pass before firmware builds
- Triggered on push to `dev`, `main`, `feature/*`
- Pull requests require passing CI

---

*For detailed API documentation, see [docs/API.md](docs/API.md)*
*For troubleshooting, see [docs/MANUAL.md](docs/MANUAL.md#j-troubleshooting)*
