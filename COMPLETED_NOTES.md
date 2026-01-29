# Completed Features & Investigations Archive

This file archives completed work items that were previously in FUTURE_NOTES.md.
Keeping these notes preserves implementation details and design decisions for historical reference.

---

## [v3.0.7] - January 2026

### ✅ Audio: I2S Driver Migration

**Completed**: January 2026

Migrated `audio_beep.cpp` from legacy ESP32 I2S driver API to new standard:
- Uses `driver/i2s_std.h` (Arduino-ESP32 3.x standard)
- Functions: `i2s_new_channel()`, `i2s_channel_init_std_mode()`, `i2s_channel_enable()`, `i2s_channel_write()`
- No deprecated `driver/i2s.h` imports remain
- Clean build with no deprecation warnings

---

### ✅ OBD-II / ELM327 Speed Integration

**Completed**: v3.0.7 (January 2026)

Full implementation of ELM327 BLE OBD-II adapter support for vehicle speed data.

**Implementation Details**:
- Client reuse pattern (no runtime deleteClient)
- RSSI guard (-85 dBm threshold)
- Exponential backoff (5→10→20→40→60s)
- 12s delay after V1 connect (radio contention mitigation)

**Files**: `src/obd_handler.cpp`, `/gps` page UI

**Hardware Recommendation**: Veepeak OBDCheck BLE+ (~$22)

**ELM327 Command Sequence**:
```
AT Z          # Reset
AT E0         # Echo off  
AT SP 0       # Auto-detect protocol
01 0D         # Request vehicle speed
Response: 41 0D 50 → Speed = 80 km/h
```

---

### ✅ Proxy Path Verification

**Verified**: January 20, 2026

- **Notify routing**: `forwardToProxyImmediate()` and `processProxyQueue()` dispatch by charUUID (B2CE→short, B4E0→long)
- **Long notify**: pProxyNotifyLongChar used for B4E0 responses (voltage, etc.)
- **Write routing**: `ProxyWriteCallbacks::onWrite()` routes B8D2→pCommandCharLong, others→sendCommand()
- **Direct writes in callbacks**: Works fine - NimBLE's writeValue() is non-blocking
- **32-byte cap**: V1 protocol limit, not MTU issue
- Architecture is sound with no reliability issues.

---

### ✅ Windows Build Fallbacks

**Documented**: See [docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md)

- Primary: `./build.sh --all` (auto-detects Windows, uses `waveshare-349-windows` env)
- Fallback: manual `pio run -e waveshare-349-windows -t upload/uploadfs`

---

### ✅ Arrow Behavior (Intentional Design)

**Status**: Working as intended

- Arrows render directly from V1 display packets (no debounce/smoothing)
- Matches real V1 display behavior exactly
- No user complaints about twitchiness (January 2026)

---

### ✅ Timing Constants (Intentional Design)

**Status**: Working as intended

- BAND_GRACE_MS (100ms) defined locally in display.cpp near usage
- Constants-near-usage is good embedded practice
- Values stable since January 2026 (50ms display refresh, 100ms grace/decay)

---

### ✅ Audio Beep / Voice Alerts

**Completed**: January 2026

Full TTS voice alert system with ES8311 DAC:
- Speaks band, frequency, direction on new alerts
- Bogey count announcement when multiple alerts active
- "Warning: Volume Zero" plays when V1 volume is 0
- Speaker volume control via web UI (0-100%)
- Mu-law compressed audio files on LittleFS (~500KB total)
- Settings: voice mode (off/band/freq/band+freq), direction toggle, bogey count toggle
- Amp warm-keeping for responsive consecutive announcements

---

## Design Decisions (Intentionally Not Implemented)

### ❌ Watchdog Timer - Intentionally Avoided

**Reasoning**:
- BLE can block 15-20s on connection timeout (would trigger false resets)
- WiFi web server has long-running requests
- Current BLE state machine with exponential backoff handles stuck states well
- Adding watchdog would introduce more failure modes than it solves

---

### ❌ rxBuffer Ring Buffer Optimization - WONTFIX

**Reasoning**:
- Current `vector.erase()` is O(n) but n is tiny (≤256 bytes, typically 20-60)
- ESP32 shifts 256 bytes in ~10-20 µs; at 20 Hz = 0.02-0.04% CPU (negligible)
- Ring buffer would add complexity for marginal gain
- No latency issues observed in real-world use

---

### ⏸️ Unit Tests for PacketParser - Low Priority

**Reasoning**:
- REPLAY_MODE already exercises parser with real V1 packet captures
- Real hardware testing covers edge cases
- Current coverage sufficient for single-developer project
- Consider only if handing off to other maintainers

---

*Archive created: January 29, 2026*
