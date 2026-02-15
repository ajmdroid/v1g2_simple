# Changelog

All notable changes to the V1-Simple project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [4.0.0-dev] - 2026-02-15

### Added

**Quality + Runtime Hardening**
- Expanded native unit-test coverage across lockout, camera, display, OBD, and parser modules (`pio test -e native`).
- Lockout runtime stack fully integrated (capture, learner, enforcer, store/index, zone APIs).
- Camera runtime/index/loader/event-log path active with bounded cadence and status/event APIs.

- **Security Warning**: Default password warning banner in web UI
  - Shows on all pages when using factory default password
  - Dismissible per session
  - Links to Settings page for easy password change

**WiFi Client Mode**
- Connect to external WiFi networks for internet access
- Maintains AP mode for device access while connected

### Changed
- **CI/CD**: Build now runs unit tests before firmware compilation
- **Input Validation**: Added proxy_name length limit (32 chars)
- **API**: Added `isDefaultPassword` flag to `/api/settings` response
- **Docs**: API routes/docs aligned to active firmware handlers; stale/internal references removed.

### Fixed
- All previously documented bugs verified as already fixed in codebase

### Security
- Default password warning encourages users to change factory credentials

---

## [3.0.7] - Previous Release

### Features
- Full V1 BLE connectivity with packet parsing
- Auto-lockout with GPS geofencing
- OBD speedometer integration
- V1 profile management with auto-push
- Custom display themes and colors
- Camera alert database support
- SD card backup functionality
- Runtime diagnostics and perf counters

### Technical
- ESP32-S3 on Waveshare 3.49" display
- BLE dual-connection (V1 + OBD)
- FreeRTOS multi-tasking
- LittleFS + SD card storage
- Responsive SvelteKit web interface

---

## Version History

| Version | Date | Highlights |
|---------|------|------------|
| 4.0.0-dev | 2026 | Development pre-release with API/runtime refactors |
| 3.0.7 | 2026 | Quality baseline before 4.x refactors |
| 3.0.x | 2024 | Camera alerts, OBD integration |
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
*For troubleshooting, see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)*
