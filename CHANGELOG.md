# Changelog

All notable changes to the V1-Simple project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [4.1.3] - 2026-04-13

### Fixed
- OTA: stop TLS client before manifest fetch to prevent stale session state causing handshake failures (HTTP -1) on redirect host.
- OTA: replace banned `NimBLEDevice::deinit()` with safe disconnect + scan stop during pre-flash BLE shutdown.
- OTA: distinguish TLS/network errors from HTTP errors in user-facing messages.
- OTA: open NVS namespace read-write in `loadPendingFilesystemUpdate` to suppress ESP-IDF NOT_FOUND error log on first boot.

---

## [4.1.2] - 2026-04-13

### Changed
- OTA `auto` target mode: compares installed filesystem SHA-256 against manifest to skip unnecessary filesystem downloads. UI shows "Firmware + filesystem" or "Firmware only" accordingly.
- OTA start endpoint default target changed from `both` to `auto`.

---

## [4.1.1] - 2026-04-13

### Added

**OTA Update System**
- Over-the-air firmware and filesystem updates via GitHub Releases.
- Two-phase update: firmware flashes first, filesystem follows on next boot.
- SHA-256 binary validation of downloaded artifacts.
- LCD progress display during download (web UI drops on reboot).
- Cancel support during download via `/api/ota/cancel`.
- SNTP time sync before TLS handshake for certificate validation.
- Automatic rollback if new firmware fails to boot.
- New API endpoints: `/api/version`, `/api/ota/status`, `/api/ota/check`, `/api/ota/start`, `/api/ota/cancel`.

**Observability**
- `parseResyncs` counter for BLE framing-level resync events (bad length, size, or end marker).
- `parse_resyncs_delta` wired to hardware metric catalog and soak KV export.

**Power**
- `powerOffSdLog` dev toggle for power-off SD diagnostics.

**Testing**
- Comprehensive display test replacing 5-step color preview.
- Error injection tests for AutoPush and QuietCoordinator failure paths.
- Restore API guardrail test.

### Changed
- ALP terminology renamed: scan → detection, armed → defense (matches manufacturer conventions).
- OBD module yields radio to V1 during reconnection; defers OBD BLE scan while V1 connection is in progress.
- BLE timing state unified to `uint32_t` across all modules.
- Heap sampling cadence reduced to every 8th loop iteration.
- OFR glyph cache and text-width cache depth increased.
- AlpSdLogger injected via `begin()` instead of extern global.
- `test.sh` gains `--ip` flag, honors `DEVICE_PORT`/`METRICS_URL` env vars.

### Fixed
- `clampWifiModeValue` uses allowlist approach for gapped `WiFiModeSetting` enum.
- `attemptNvsRecovery` returns false when no space was actually freed.
- Slot colors sanitized in both restore path and API setter.
- `resetDisplaySettings` volume color defaults match constructor and NVS.
- ALP EN pin and unused TX pin assignment removed.
- Stale gun ID cleared on new ALP alert entry.
- ALP SD CSV column alignment corrected.
- Dead code removed: 3 confirmed dead symbols, dead `subscribeBleNotifications()` path, dead classic frequency branch.

### Security
- OTA downloads use TLS (no cert pinning — GitHub CA migration makes pinning fragile). Binary integrity verified via SHA-256.

---

## [4.0.1] - 2026-04-04

### Fixed
- Web installer merged firmware now uses the correct `dio` flash mode for ESP Web Tools on the Waveshare ESP32-S3-Touch-LCD-3.49, preventing browser-flashed boards from appearing dead after install.
- Installer entrypoints now fail over to a secure hosted ESP Web Tools URL when the custom domain is not in a secure context, avoiding browser-side `Serial access not allowed` failures caused by site HTTPS issues.

### Changed
- Added dedicated GitHub Actions workflows to deploy installer HTML changes and manually refresh installer assets without reusing an existing release tag.
- Documentation now points users at the secure hosted installer during the 4.0.1 hotfix rollout.

## [4.0.0] - 2026-04-01

> Note: no tagged releases were published between `3.0.7` and `4.0.0`;
> ongoing work during that gap landed directly in the dev cycle summarized here.

### Added

**Module Extraction (625 commits since 2026-02-15)**
- Extracted 15 module directories under `src/modules/` (139 files, ~17,500 lines).
- Loop phase orchestration extracted to `main_loop_phases.cpp` (~190 lines) with 10 phase-router modules (`LoopIngestModule`, `LoopConnectionEarlyModule`, `LoopDisplayModule`, etc.).
- Core service splits: `ble_runtime.cpp` (511 lines), `packet_parser_alerts.cpp` (582 lines), `settings_restore.cpp` (782 lines).
- Boot-time helpers extracted to `main_boot.cpp` (248 lines).
- Persist save state machines extracted to `main_persist.cpp` (445 lines).
- WiFi subsystem modularized into dedicated runtime, policy, cadence, and visual-sync modules.
- BLE connection runtime and state dispatch modules extracted with Providers DI pattern.
- Speed-volume runtime, speaker-quiet sync, and voice-speed sync modules added.
- `SystemEventBus` and `PeriodicMaintenanceModule` for cross-cutting concerns.
- `DebugPerfFilesService` extracted from debug API for perf-file management.
- 8 CI contract scripts enforcing architectural invariants.

**Quality + Runtime Hardening**
- Expanded to 76 native test suites, 960 test cases (`pio test -e native`).
- Drive-scenario integration tests (15 scenarios).
- OBD runtime module integrated (speed polling, reconnect, scan-from-UI).
- Heap safety hardened with RAII ownership and teardown guards.

- **Security Warning**: Default password warning banner in web UI
  - Shows on all pages when using factory default password
  - Dismissible per session
  - Links to Settings page for easy password change

**WiFi Client Mode**
- Connect to external WiFi networks for internet access
- Maintains AP mode for device access while connected

**Performance**
- Perf CSV schema expanded with subsystem timing (OBD, display, BLE).
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
- **Docs**: Full documentation audit — MANUAL.md, API.md aligned to code.

### Fixed
- WiFi STA config recovery when NVS keys are missing (SD secret fallback).
- Display flush contract stabilized against line-offset drift.
- Settings flushed reliably on shutdown.

### Security
- Default password warning encourages users to change factory credentials

---

## [3.0.7] - Previous Release

### Features
- Full V1 BLE connectivity with packet parsing
- OBD-II speed source via BLE
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
| 4.1.3 | 2026-04-13 | OTA TLS fix, safe BLE shutdown, better error messages |
| 4.1.2 | 2026-04-13 | OTA intelligent filesystem skip via SHA-256 comparison |
| 4.1.1 | 2026-04-13 | OTA update system, ALP terminology rename, BLE resync observability |
| 4.0.1 | 2026-04-04 | Web installer hotfix: corrected merged flash mode, secure hosted fallback |
| 4.0.0 | 2026-04-01 | Modular architecture, 141 module files, 960 tests, CI contracts |
| 3.0.7 | 2026 | Quality baseline before 4.x refactors |
| 3.0.x | 2024 | Speed source improvements |
| 2.x.x | 2024 | Profiles, display themes |
| 1.x.x | 2023 | Initial release, basic V1 display |

---

## Upgrade Notes

### From 4.0.0 to 4.0.1

This hotfix is recommended for anyone installing from the browser-based flasher.

Recommended post-upgrade actions:
1. Use the hosted fallback installer until the custom-domain HTTPS configuration is fully healthy again.
2. If a 4.0.0 browser flash left the board non-booting, recover with USB erase/reflash or re-install with 4.0.1 once the refreshed installer assets are published.

### From 3.x to 4.0.0

API/behavior changes exist versus 3.0.7; validate integrations before production use.

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
