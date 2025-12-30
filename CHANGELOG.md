# Changelog

All notable changes to this project will be documented in this file.

---

## Unreleased (v1.0.9)

### Added
- Band label font: Native 24pt FreeSansBold for cleaner rendering
- Direction arrow black outlines (matches V1 reference)
- Custom GFX font generation via Adafruit fontconvert
- Web UI toggles to hide WiFi icon and profile indicator after timeout
  - Hide WiFi Icon: shows briefly on connect, then clears
  - Hide Profile Indicator: shows on profile change, then clears
- README note about `uploadfs` for web UI changes

### Changed
- Direction arrows scaled up ~22% with shallower angles to match V1
- Splash screen reduced from 4s to 2s, properly centered fullscreen
- Web UI GitHub link updated to this repo
- Bogey counter scaled to match frequency counter size (2.2x)
- Signal bars: larger (56x14px), repositioned for better spacing

### Fixed
- Band label pixelation (was using scaled 10pt, now native 24pt)
- Profile indicator purple line artifact (fixed clear width)

### Notes
- Still investigating: radar mute flicker with JBV1 auto-mute

---

## v1.0.8 (2025-12-29)

### Added
- Serial logging to SD card with web UI viewer (`/seriallog`)
- WiFi NAT/NAPT router mode (AP+STA passthrough)
- Multi-network WiFi support (up to 3 saved networks)
- NTP time sync

### Fixed
- XSS security vulnerability in web UI
- Credential obfuscation for stored passwords
- Fast reconnect with SD storage
- BLE proxy delay
- Laser flicker issue
- Various crash fixes and code cleanup

---

## Development Log

### December 30, 2025 - Display Polish & UI Updates

**Band Label Font Enhancement**
- Replaced scaled 10pt font with native 24pt FreeSansBold
- Generated custom GFX font using Adafruit fontconvert tool
- Properly positioned for Waveshare 3.49" display (startY=55, spacing=43)

**Direction Arrow Improvements**
- Scaled all arrows up ~22% for better visibility
- Widened arrows with shallower angles to match V1 reference
- Added black outlines to all arrows (front, side, rear)
- Increased gap between arrows (13px) for better visual separation
- Final dimensions: Top 125x62px, Bottom 125x40px, Side bar 66px with 28x22 heads

**Other Changes**
- Splash screen duration: 4s → 2s
- Web UI GitHub link updated to ajmdroid/v1g2_simple

---

### December 27, 2024 - Documentation & Cleanup

**Documentation Overhaul**
- Complete README.md rewrite with step-by-step installation
- Created RDF_POST.md for community sharing
- Zero-to-working guide for non-developers
- Heavy attribution to Kenny Garreau's foundational work

**Code Comment Cleanup**
- Commented out touch/BLE debug logging (cleaner serial output)
- Added comprehensive file headers to all major classes
- Architecture notes and usage examples added

**Touch Handler Integration**
- CST816T touch controller with correct I2C address (0x15)
- Hardware reset support via GPIO 21
- 50ms debounce logic
- Tap-to-mute functionality

**NimBLE Stabilization**
- Downgraded NimBLE-Arduino 2.3.2 → 2.2.3 for dual-role stability
- Matched Kenny's scan configuration
- FreeRTOS task for advertising restart

---

## Roadmap

- [ ] Web UI color customization improvements
- [ ] RDF boot splash
- [ ] Web UI log sorting
- [ ] GPS support/lockouts (future)
- [x] Tap-to-mute feature
- [x] V1 profiles CRUD
- [x] 3-slot auto-push system
- [x] Split LilyGO support (separate project)
