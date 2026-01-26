# Changelog

All notable changes to the V1-Simple project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [3.1.0] - 2024-XX-XX

### Added

**Enterprise Quality Initiative**
- **Unit Test Framework**: 40+ automated tests using Unity test framework
  - Haversine distance calculations (10 tests)
  - V1 packet parser (30 tests)
  - Run via `pio test -e native`

- **Error Codes**: Structured error code system (`src/error_codes.h`)
  - Categorized codes: BLE, GPS, Storage, WiFi, V1, System
  - Helper macros for error classification
  - Human-readable error strings

- **Security Warning**: Default password warning banner in web UI
  - Shows on all pages when using factory default password
  - Dismissible per session
  - Links to Settings page for easy password change

**WiFi Client Mode**
- Connect to external WiFi networks for internet access
- Enables OSM camera sync and future OTA updates
- Maintains AP mode for device access while connected

**OpenStreetMap Camera Sync**
- Download speed cameras from OSM via Overpass API
- Configurable search radius (up to 100km)
- Requires WiFi client connection to internet

### Changed
- **CI/CD**: Build now runs unit tests before firmware compilation
- **Input Validation**: Added proxy_name length limit (32 chars)
- **API**: Added `isDefaultPassword` flag to `/api/settings` response

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
- Debug logging to LittleFS

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
| 3.1.0 | TBD | Enterprise quality, WiFi client, OSM sync |
| 3.0.7 | 2024 | Quality baseline, all critical bugs fixed |
| 3.0.x | 2024 | Camera alerts, OBD integration |
| 2.x.x | 2024 | Auto-lockout, profiles |
| 1.x.x | 2023 | Initial release, basic V1 display |

---

## Upgrade Notes

### From 3.0.x to 3.1.0

**No breaking changes.** Direct upgrade supported.

Recommended post-upgrade actions:
1. Change default WiFi password if not already done
2. Review new security warning banner
3. Try WiFi client mode for OSM camera sync

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
pio test -e native -f test_haversine
```

### Building Firmware
```bash
# Build only
pio run -e waveshare-349

# Build and upload
pio run -t upload -e waveshare-349

# Build web interface first
cd interface && npm run build && cd ..
cp -r interface/build/* data/
```

### CI Status
- Tests must pass before firmware builds
- Triggered on push to `dev`, `main`, `feature/*`
- Pull requests require passing CI

---

*For detailed API documentation, see [docs/API.md](docs/API.md)*
*For troubleshooting, see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)*
