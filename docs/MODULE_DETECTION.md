# GPS and OBD Module Safety Features

## Overview

GPS and OBD-II modules are **optional add-ons** that enhance functionality but aren't required for basic radar detector operation. The system intelligently detects their presence and disables unused features to avoid wasting resources.

## Default Behavior

### GPS Module
- **Default**: Disabled (opt-in)
- **Setting**: `gpsEnabled = false`
- **Detection**: 60 second timeout
- **Action if not found**: Permanently disabled until setting re-enabled

### OBD-II Module
- **Default**: Disabled (opt-in)
- **Setting**: `obdEnabled = false`
- **Detection**: TBD (when OBD implementation added)
- **Action if not found**: Permanently disabled until setting re-enabled

## User Experience

### Scenario 1: No GPS Module Connected
```
[Boot] GPS disabled in settings - skipping initialization
[Display] No GPS icon shown
[Resources] GPS handler not started, Serial2 unused
```

### Scenario 2: GPS Enabled, Module Present
```
[Boot] GPS enabled, initializing...
[GPS] Module detected
[GPS] Adafruit PA1616S initialized (10Hz, GPS+GLONASS+Galileo)
[GPS] Searching for fix... (Sats: 0)
[GPS] Searching for fix... (Sats: 4)
[GPS] Fix: 37.774900, -122.419400 | HDOP: 1.2 | Sats: 8
[Display] GPS icon shown (green when locked)
```

### Scenario 3: GPS Enabled, Module NOT Present
```
[Boot] GPS enabled, initializing...
[GPS] Searching for fix... (Sats: 0)
[GPS] Searching for fix... (Sats: 0)
... 60 seconds pass ...
[GPS] Module NOT detected (timeout) - GPS disabled
[Settings] Auto-disabling GPS (module not found)
[Display] No GPS icon shown
[Resources] GPS handler stops checking serial port
```

## Settings Storage

### Preferences (NVS)
```cpp
settings.gpsEnabled = false;  // Default: off
settings.obdEnabled = false;  // Default: off
```

### JSON Backup (SD/LittleFS)
```json
{
  "gpsEnabled": false,
  "obdEnabled": false
}
```

## Detection Logic

### GPS Detection (gps_handler.cpp)
```cpp
bool GPSHandler::update() {
  // Skip if detection already failed
  if (detectionComplete && !moduleDetected) {
    return false;  // No-op, don't waste CPU cycles
  }
  
  // Read serial data
  char c = GPS.read();
  
  if (GPS.newNMEAreceived()) {
    // Any NMEA sentence = module present
    if (!detectionComplete) {
      moduleDetected = true;
      detectionComplete = true;
      Serial.println("[GPS] Module detected");
    }
  }
  
  // Timeout check
  if (!detectionComplete && millis() - detectionStartMs > 60000) {
    detectionComplete = true;
    moduleDetected = false;
    Serial.println("[GPS] Module NOT detected (timeout)");
    // Auto-disable in settings
    settingsManager.setGpsEnabled(false);
  }
}
```

### Detection Timeline
```
Time     Event
────────────────────────────────────────
0s       GPS.begin() - start detection timer
0-60s    Check for NMEA sentences on Serial2
         - If data received: moduleDetected = true
         - If no data: keep checking
60s      Timeout - mark as NOT detected
60s+     Stop checking serial port (waste prevention)
```

## Resource Conservation

### Before Detection (Disabled)
- Serial2 UART: **Not initialized**
- RAM usage: **0 bytes** (no GPSHandler object)
- CPU usage: **0%** (no polling)

### After Detection (Module Found)
- Serial2 UART: **Active** (9600 baud)
- RAM usage: **~2 KB** (GPSHandler + buffers)
- CPU usage: **<1%** (10Hz NMEA parsing)

### After Detection (Module NOT Found)
- Serial2 UART: **Stopped** (gpsSerial.end())
- RAM usage: **Minimal** (skeleton object only)
- CPU usage: **0%** (early return in update())

## Integration Example

### main.cpp Setup
```cpp
#include "gps_handler.h"

GPSHandler* gps = nullptr;

void setup() {
  // Load settings
  settingsManager.begin();
  
  // Only initialize GPS if enabled
  if (settingsManager.isGpsEnabled()) {
    gps = new GPSHandler();
    gps->begin();
    Serial.println("[Setup] GPS initialization started");
  } else {
    Serial.println("[Setup] GPS disabled in settings");
  }
}

void loop() {
  // Only update if initialized
  if (gps != nullptr) {
    gps->update();
    
    // Check if detection failed
    if (gps->isDetectionComplete() && !gps->isModuleDetected()) {
      // Auto-disable and cleanup
      Serial.println("[Main] GPS module not found, disabling");
      delete gps;
      gps = nullptr;
      settingsManager.setGpsEnabled(false);
    }
    
    // Use GPS data if valid
    if (gps != nullptr && gps->hasValidFix()) {
      GPSFix fix = gps->getFix();
      // Use fix.latitude, fix.longitude, fix.speed_mps, etc.
    }
  }
}
```

## Web UI Controls

### Settings Page (/settings)
```html
<div class="form-control">
  <label class="label cursor-pointer">
    <span class="label-text">Enable GPS</span>
    <input type="checkbox" class="toggle" id="gpsEnabled">
  </label>
  <span class="label-text-alt">
    Requires GPS module connected to GPIO17/18
  </span>
</div>

<div class="form-control">
  <label class="label cursor-pointer">
    <span class="label-text">Enable OBD-II</span>
    <input type="checkbox" class="toggle" id="obdEnabled">
  </label>
  <span class="label-text-alt">
    Requires OBD-II adapter connected to CAN bus
  </span>
</div>
```

### REST API Endpoints
```
GET  /api/settings/gps
     → { "enabled": false, "detected": false }

POST /api/settings/gps
     { "enabled": true }
     → Enables GPS, starts detection

GET  /api/settings/obd
     → { "enabled": false, "detected": false }

POST /api/settings/obd
     { "enabled": true }
     → Enables OBD, starts detection
```

## Benefits

1. **No Resource Waste**
   - Unused modules don't consume CPU/RAM
   - Serial ports not initialized if disabled
   - No background polling for non-existent hardware

2. **User-Friendly**
   - Auto-detects modules within 60 seconds
   - Clear serial messages explain what's happening
   - Auto-disables if not found (no repeated errors)

3. **Safe Defaults**
   - GPS/OBD off by default (opt-in)
   - Device works perfectly without add-ons
   - No surprises or unexpected behavior

4. **Easy Troubleshooting**
   - Serial monitor shows detection progress
   - Web UI shows module status
   - Settings persist across reboots

## Future Enhancements

- [ ] Shorter detection timeout (30s instead of 60s)
- [ ] Retry detection on manual re-enable
- [ ] Module health monitoring (detect disconnection)
- [ ] Web UI notification when module not found
- [ ] OBD-II detection implementation (when CAN support added)
- [ ] GPS/OBD status on main display
