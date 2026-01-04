# RD Simple - Development Roadmap

> **Working document** - Captures design decisions and future plans  
> **Date:** January 3, 2026

---

## Project Direction

Renaming concept: **"RD Simple"** (Radar Detector Simple) to support multiple detectors.

### Supported Detectors
- **Valentine One Gen2** - Current, working
- **Uniden R8** - In development

---

## Build Strategy

**Decision:** Separate firmware builds via build flags (not unified runtime)

```ini
[env:v1-simple]     # Valentine One
build_flags = -D DETECTOR_V1=1

[env:r8-simple]     # Uniden R8
build_flags = -D DETECTOR_R8=1
```

**Rationale:**
- Cleaner code paths (no runtime if/else)
- Easier debugging (isolated stack traces)
- Independent testing per detector
- BLE stack configured for one purpose
- Simpler user experience (flash what you have)

---

## Architecture Ideas

### R8 BLE Proxy (High Priority)

Transparent proxy so R8 apps work through the display device:

```
┌──────┐     BLE      ┌─────────┐      BLE       ┌───────┐
│  R8  │ ◀──────────▶ │ ESP32   │ ◀────────────▶ │ Phone │
│      │  (Client)    │ Display │   (Server)     │ R8App │
└──────┘              │ + Local │                └───────┘
                      └─────────┘
```

**Benefits:**
- Phone stays in pocket
- Local display on dash
- Range extension (ESP closer to R8)
- Multiple clients possible (future)

**Implementation:** Mirror R8 service UUIDs, forward notifications both ways.  
**Reference:** V1 proxy code in `ble_client.cpp`

---

### Protocol Bridge (Future/Complex)

Convert R8 data to V1 format so JBV1 app works with R8:

```
R8 Alert → UnifiedAlert → V1 Packet → JBV1 App
JBV1 Cmd → V1 Parser → UnifiedCmd → R8 Command
```

**Status:** Interesting idea but likely too much latency/processing for real-time use.  
**Revisit:** After R8 proxy is working, if there's demand.

---

### Unified Data Model (For Shared Display)

```cpp
struct UnifiedAlert {
    DetectorType source;     // V1, R8
    Band band;               // X, K, KA, LASER
    int strength;            // Normalized 0-100
    Direction direction;     // FRONT, SIDE, REAR
    float freqGHz;
    bool muted;
};
```

**Use case:** Shared display code that works with either detector.  
**When:** After both detectors are stable individually.

---

## File Organization

### Current
```
src/           # V1 code (main build)
r8/src/        # R8 code (separate build)
```

### Future (when patterns emerge)
```
src/           # V1 specific
r8/src/        # R8 specific  
shared/        # Common utilities
├── display_common.h
├── wifi_manager.cpp
├── settings.cpp
└── ...
```

---

## Development Phases

### Phase 1: R8 Modular Code ✅
- [x] r8_parser.h/cpp - Alert/status parsing
- [x] r8_client.h/cpp - BLE connection management
- [x] r8_display.h/cpp - Visual rendering
- [x] r8_main.cpp - Clean entry point
- [x] Build: `pio run -e r8-modular`

### Phase 2: R8 Stability (Current)
- [x] Test reconnection reliability - **FAST! Much faster than V1**
- [ ] Handle edge cases (R8 off, out of range, etc.)
- [ ] Verify bonding persistence across reboots
- [ ] Add WiFi/web config (port from V1)

### Phase 3: R8 BLE Proxy
- [ ] Create BLE server with R8 UUIDs
- [ ] Forward notifications (R8 → phone)
- [ ] Forward writes (phone → R8)
- [ ] Handle proxy-side bonding
- [ ] Test with official R8 app

### Phase 4: Polish
- [ ] Rename environments to v1-simple/r8-simple
- [ ] Shared display primitives
- [ ] Documentation cleanup
- [ ] Web UI for R8 (if needed)

---

## R8 Protocol Reference

### BLE UUIDs
| UUID | Name |
|------|------|
| `18424398-7CBC-11E9-8F9E-2A86E4085A59` | Service |
| `6EB675AB-8BD1-1B9A-7444-621E52EC6823` | Alert (notify) |
| `6C290D2E-1C03-ACA1-AB48-A9B908BAE79E` | Status (notify) |
| `5D1A55E0-AC5E-11E9-A2A3-2A2AE2DBCCE4` | Settings (r/w) |
| `B27BE4B4-7C65-11E9-8F9E-2A86E4085A59` | Command Service |
| `4DC35292-7C66-11E9-8F9E-2A86E4085A59` | Command Write |

### Alert Format
```
1,00,KA,8,194,34.7470,S,1&0&0&0
│ │  │  │  │    │     │
│ │  │  │  │    │     └── Direction (F/S/R)
│ │  │  │  │    └── Frequency GHz
│ │  │  │  └── Raw signal
│ │  │  └── Strength (0-8)
│ │  └── Band
│ └── Index
└── Count (0 = clear)
```

### Key Differences from V1
| V1 | R8 |
|----|-----|
| Binary protocol | ASCII strings |
| No pairing | Just Works pairing |
| Public address | Random address |
| Scan to reconnect | Direct connect (bonded) |
| Strength 0-6 | Strength 0-8 |

---

## Open Questions

1. **R8 command protocol** - Need to reverse engineer for mute/settings
2. **Proxy bonding** - How does phone bond to proxy vs R8?
3. **Multiple R8s?** - Does anyone run multiple R8s?
4. **R8 app compatibility** - Which apps to test with?

---

## Notes

*Add notes here during development...*

