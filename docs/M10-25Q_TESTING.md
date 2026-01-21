# Testing with M10-25Q GPS Module

## Hardware Setup

### Wiring (Same as PA1616S)
```
M10-25Q    →    ESP32-S3
─────────────────────────
TX         →    GPIO17 (RX)
RX         →    GPIO18 (TX)
V          →    3.3V
GND        →    GND
SCL/SDA    →    (Optional - for compass)
```

### Module Differences

| Feature | M10-25Q | PA1616S |
|---------|---------|---------|
| Chipset | u-blox M10 (10th gen) | MediaTek PA1616S |
| Constellations | GPS+GLONASS+Galileo+BeiDou | GPS+GLONASS+Galileo+QZSS |
| Update Rate | 10Hz max | 10Hz max |
| Power | ~20mA | ~18mA |
| TTFF (cold) | ~24s | ~30s |
| Extras | QMC5883L compass (I2C) | Backup battery for RTC |
| NMEA Output | Yes (standard) | Yes (PMTK protocol) |

## Software Configuration

### Option 1: Keep Using Adafruit GPS (Try First)
The M10-25Q outputs standard NMEA sentences, so the Adafruit GPS library *might* work.

**Do this first** - no code changes needed:
1. Wire M10-25Q as shown above
2. Upload existing code
3. Check serial monitor for GPS fix

If you see `[GPS] Searching for fix... (Sats: X)` and it eventually gets a fix, you're good!

### Option 2: Switch to TinyGPSPlus (If Needed)
If Adafruit GPS doesn't work, TinyGPSPlus is guaranteed to work with u-blox modules.

**Edit gps_handler.h:**
```cpp
// Uncomment to use TinyGPSPlus (for M10-25Q or other u-blox modules)
#define USE_TINYGPS  // <-- Remove the // to enable
```

**Rebuild and upload:**
```bash
cd /Users/ajmedford/v1g2_simple
pio run -t upload -t monitor
```

You should see:
```
[GPS] TinyGPSPlus initialized for M10-25Q (NMEA parser)
[GPS] Wiring: TX->GPIO18, RX->GPIO17
[GPS] Searching for fix... (Sats: 0)
[GPS] Searching for fix... (Sats: 4)
[GPS] Searching for fix... (Sats: 7)
[GPS] Fix: 37.774900, -122.419400 | HDOP: 1.2 | Sats: 8 | Speed: 0.0 m/s
[GPS] Time: 2026-01-21 20:45:30 UTC
```

## Testing Checklist

### 1. Cold Start Test
- [ ] Power on with no previous GPS data
- [ ] Should get fix in ~24 seconds (outdoor, clear sky)
- [ ] Serial monitor shows satellite count increasing
- [ ] Fix accuracy: HDOP < 2.0 is good

### 2. Time Extraction Test
- [ ] GPS time matches current UTC time
- [ ] Unix timestamp is reasonable (> 1700000000)
- [ ] Time updates every second

### 3. Speed/Heading Test
- [ ] Speed shows 0.0 m/s when stationary
- [ ] Walk around with module, speed should show 1-2 m/s
- [ ] Heading changes as you turn (0-360 degrees)

### 4. Auto-Lockout Test
- [ ] Drive past a known K-band false alert location
- [ ] Check that alert is logged: `[AutoLockout] Created cluster...`
- [ ] Drive past same location again within 2 days
- [ ] Should promote after 2nd stopped hit or 4th moving hit

## Troubleshooting

### No GPS Fix
1. **Check wiring** - TX/RX are swapped (GPS TX → ESP32 RX)
2. **Check power** - M10-25Q needs 3.3V, not 5V
3. **Move outdoors** - GPS needs clear view of sky
4. **Check baud rate** - Should be 9600 (M10 default)

### GPS Fix But No Time
- Wait 30 seconds after first fix for time sync
- Check serial output for "Time: 2026-01-21..." line
- If still no time, enable `USE_TINYGPS` and rebuild

### Adafruit GPS Library Doesn't Work
- u-blox modules don't respond to PMTK commands (that's OK)
- Enable `USE_TINYGPS` in gps_handler.h
- TinyGPSPlus is specifically designed for u-blox modules

### Compass Not Working
The QMC5883L compass is optional for now. To use it later:
```cpp
// Add to platformio.ini:
lib_deps = 
    ... existing libraries ...
    mprograms/QMC5883LCompass@^1.0.0

// Wire SCL/SDA to ESP32 I2C pins:
SCL → GPIO21
SDA → GPIO20
```

## Performance Comparison

Once you get the Adafruit PA1616S, here's what to expect:

| Scenario | M10-25Q | PA1616S |
|----------|---------|---------|
| Cold start (outdoor) | ~24s | ~30s |
| Hot start (< 2hr) | ~1s | ~1s |
| Accuracy (CEP50) | 2.0m | 2.5m |
| Indoor performance | Better (stronger receiver) | Good |
| Battery backup | None (loses time) | Coin cell (keeps RTC) |
| Compass | Yes (QMC5883L) | No |

The M10-25Q is actually slightly *better* for GPS accuracy, but the PA1616S is easier to work with (smaller, integrated antenna, battery backup).

## When PA1616S Arrives

To switch back:
1. Comment out `#define USE_TINYGPS` in gps_handler.h
2. Rebuild and upload
3. Wire PA1616S same way (TX→GPIO17, RX→GPIO18)

All your lockout data will be preserved since it's stored in JSON on flash!

## Next Steps

Once GPS is working:
1. Test auto-lockout learning on a real drive
2. Add REST API endpoints for lockout management
3. Build web UI to view/edit lockout zones
4. Optional: Add compass heading display to main screen
