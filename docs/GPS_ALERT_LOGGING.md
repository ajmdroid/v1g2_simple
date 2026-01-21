# GPS Alert Logging System

## Overview

The V1 Simple auto-lockout system logs radar/laser alerts with GPS coordinates and timestamps to automatically learn false alert locations.

## Time Source: GPS vs ESP32 RTC

### GPS Time (Preferred)
- **Accuracy**: Atomic clock-based, accurate to milliseconds
- **Source**: NMEA sentences from PA1616S GPS module
- **Format**: UTC (Coordinated Universal Time)
- **Availability**: Only when GPS fix is valid
- **Usage**: `GPSHandler::getGPSTime()` returns Unix timestamp

```cpp
// Example: Get GPS time
GPSFix fix = gps.getFix();
if (fix.valid && fix.unixTime > 0) {
  time_t gpsTime = fix.unixTime;  // Accurate UTC timestamp
  Serial.printf("GPS Time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                2000 + fix.year, fix.month, fix.day,
                fix.hour, fix.minute, fix.seconds);
}
```

### ESP32 RTC (Fallback)
- **Accuracy**: Drift ~1-2 seconds per day
- **Source**: ESP32 internal RTC (set at boot)
- **Usage**: `time(nullptr)` - standard C time function
- **Fallback**: Used when GPS fix is lost

### Time Sync Strategy
```cpp
// In main.cpp loop:
if (gps.hasValidTime() && (millis() - lastTimeSyncMs > 3600000)) {
  // Sync ESP32 RTC to GPS every hour
  struct timeval tv = { .tv_sec = gps.getGPSTime(), .tv_usec = 0 };
  settimeofday(&tv, NULL);
  lastTimeSyncMs = millis();
  Serial.println("[GPS] Synced ESP32 RTC to GPS time");
}
```

## Alert Logging Data Structure

### AlertEvent (Single Alert)
```cpp
struct AlertEvent {
  float latitude;           // GPS coordinate
  float longitude;          // GPS coordinate
  Band band;                // X/K/Ka/Laser
  uint32_t frequency_khz;   // Exact frequency (e.g., 24150 for K-band)
  uint8_t signalStrength;   // 0-9 from V1 Gen2
  uint16_t duration_ms;     // How long alert lasted
  time_t timestamp;         // Unix timestamp (GPS time preferred)
  bool isMoving;            // Speed > 2 m/s when alert occurred
  bool isPersistent;        // Signal lasted >2 seconds (stationary source)
};
```

### LearningCluster (Multiple Alerts at Same Location)
```cpp
struct LearningCluster {
  String name;                        // Auto-generated name
  float centerLat, centerLon;         // Cluster center (average of all events)
  float radius_m;                     // Cluster radius (max distance from center)
  Band band;                          // Alert band
  uint32_t frequency_khz;             // Average frequency of all events
  float frequency_tolerance_khz;      // Mute range (±25 kHz)
  
  std::vector<AlertEvent> events;     // Up to 20 recent events
  
  int hitCount;                       // Total alerts detected here
  int stoppedHitCount;                // Alerts while stopped (faster promotion)
  int movingHitCount;                 // Alerts while moving (slower promotion)
  time_t firstSeen;                   // First alert timestamp
  time_t lastSeen;                    // Most recent alert timestamp
  
  int passWithoutAlertCount;          // Times passed without alert (demotion)
  time_t lastPassthrough;             // Last passthrough timestamp
  
  bool isPromoted;                    // Promoted to lockout zone
  int promotedLockoutIndex;           // Index in LockoutManager
};
```

## Memory Management

### RAM Usage
```
Single AlertEvent:        ~40 bytes
LearningCluster:          ~80 bytes + (20 events × 40 bytes) = ~880 bytes
Max 50 clusters:          50 × 880 = ~44 KB
Acceptable on ESP32-S3 with 320KB RAM
```

### Storage Strategy
1. **In-Memory**: Current learning clusters (max 50)
2. **Persistent**: `/v1profiles/auto_lockouts.json` (~50-100KB)
3. **Auto-Save**: On promotion/demotion events
4. **Manual Save**: On shutdown or user request

### Event Pruning
```cpp
// Oldest events pruned when cluster reaches 20 events
if (cluster.events.size() > MAX_EVENTS_PER_CLUSTER) {
  cluster.events.erase(cluster.events.begin());  // Remove oldest
}

// Clusters pruned when total reaches 50
if (clusters.size() >= MAX_CLUSTERS) {
  pruneOldClusters();  // Remove stale, non-promoted clusters
}
```

## Alert Recording Flow

### 1. Alert Detection (main.cpp)
```cpp
// When V1 sends alert packet
void processV1Alert(V1Alert alert) {
  GPSFix fix = gps.getFix();
  
  if (!fix.valid) {
    Serial.println("[Alert] No GPS fix, cannot log location");
    return;
  }
  
  // Record alert with GPS location and time
  autoLockouts.recordAlert(
    fix.latitude,
    fix.longitude,
    alert.band,
    alert.frequency_khz,      // Exact frequency from V1
    alert.signalStrength,     // 0-9 from V1
    alert.duration_ms,        // How long alert lasted
    gps.isMoving()            // Speed > 2 m/s
  );
}
```

### 2. Clustering (auto_lockout_manager.cpp)
```cpp
void AutoLockoutManager::recordAlert(...) {
  // Filter weak signals
  if (signalStrength < MIN_SIGNAL_STRENGTH) return;
  
  // Create event with GPS time
  AlertEvent event;
  event.timestamp = gps.getGPSTime();  // GPS time preferred
  event.isPersistent = (duration_ms > 2000);
  
  // Find existing cluster within 150m
  int clusterIdx = findCluster(lat, lon, band);
  
  if (clusterIdx >= 0) {
    addEventToCluster(clusterIdx, event);
  } else {
    createNewCluster(event);
  }
}
```

### 3. Promotion Check (auto_lockout_manager.cpp)
```cpp
bool AutoLockoutManager::shouldPromoteCluster(const LearningCluster& cluster) {
  // Stopped alerts: 2 hits (faster promotion)
  bool hasEnoughStoppedHits = cluster.stoppedHitCount >= 2;
  
  // Moving alerts: 4 hits (slower promotion)
  bool hasEnoughMovingHits = cluster.movingHitCount >= 4;
  
  if (!hasEnoughStoppedHits && !hasEnoughMovingHits) return false;
  
  // Check time window (2 days)
  time_t timeSpan = cluster.lastSeen - cluster.firstSeen;
  if (timeSpan > (2 * 24 * 3600)) return false;
  
  // Require alerts on ≥2 different days
  if (countUniqueDays(cluster.events) < 2) return false;
  
  return true;
}
```

## JSON Storage Format

### Example: auto_lockouts.json
```json
{
  "clusters": [
    {
      "name": "K-37.7749,-122.4194",
      "centerLat": 37.7749,
      "centerLon": -122.4194,
      "radius_m": 120.5,
      "band": 1,
      "frequency_khz": 24150,
      "frequency_tolerance_khz": 25,
      "hitCount": 5,
      "stoppedHitCount": 4,
      "movingHitCount": 1,
      "firstSeen": 1705881600,
      "lastSeen": 1706054400,
      "passWithoutAlertCount": 0,
      "lastPassthrough": 0,
      "isPromoted": true,
      "promotedLockoutIndex": 2,
      "events": [
        {
          "lat": 37.7748,
          "lon": -122.4195,
          "band": 1,
          "freq": 24150,
          "signal": 7,
          "duration": 3500,
          "time": 1705881600,
          "moving": false,
          "persistent": true
        },
        // ... up to 19 more events
      ]
    }
  ]
}
```

## Performance Considerations

### Update Frequency
- **GPS updates**: 10Hz (100ms refresh)
- **Alert logging**: On-demand (when V1 sends alert packet)
- **Promotion check**: Every 60 seconds (main loop timer)
- **JSON save**: On promotion/demotion only (not every alert)

### Typical Usage
```
50 clusters × 20 events = 1000 total logged alerts
Average cluster lifetime: 30 days
Storage writes: ~5-10 per day (promotions/demotions)
Flash wear: Minimal (LittleFS wear leveling)
```

## Integration Example

### main.cpp Integration
```cpp
#include "gps_handler.h"
#include "auto_lockout_manager.h"

GPSHandler* gps = nullptr;
AutoLockoutManager autoLockouts;

void setup() {
  // Initialize GPS
  gps = new GPSHandler();
  gps->begin();
  
  // Load learning data
  autoLockouts.loadFromJSON("/v1profiles/auto_lockouts.json");
  
  // Sync ESP32 RTC to GPS on first fix
  if (gps->hasValidTime()) {
    struct timeval tv = { .tv_sec = gps->getGPSTime(), .tv_usec = 0 };
    settimeofday(&tv, NULL);
  }
}

void loop() {
  // Update GPS (non-blocking)
  gps->update();
  
  // Sync ESP32 RTC to GPS periodically
  static uint32_t lastSyncMs = 0;
  if (gps->hasValidTime() && (millis() - lastSyncMs > 3600000)) {
    struct timeval tv = { .tv_sec = gps->getGPSTime(), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    lastSyncMs = millis();
  }
  
  // Check for promotion/demotion (every 60 seconds)
  static uint32_t lastUpdateMs = 0;
  if (millis() - lastUpdateMs > 60000) {
    autoLockouts.update();
    autoLockouts.saveToJSON("/v1profiles/auto_lockouts.json");
    lastUpdateMs = millis();
  }
  
  // Process V1 alerts...
}
```

## Debugging

### Serial Monitor Output
```
[GPS] Fix: 37.774900, -122.419400 | HDOP: 1.2 | Sats: 8 | Speed: 15.4 m/s
[GPS] Time: 2026-01-21 18:30:45 UTC
[AutoLockout] Created cluster 'K-37.7749,-122.4194' at (37.774900, -122.419400) freq: 24150 kHz
[AutoLockout] Added event to cluster 'K-37.7749,-122.4194' (hits: 2 [2 stopped/0 moving], radius: 85m)
[AutoLockout] ✓ PROMOTED 'K-37.7749,-122.4194' to lockout zone
```

### Web UI Query
```
GET /api/auto-lockouts
Returns JSON with all learning clusters and their status
```

## Summary

- ✅ GPS provides accurate UTC time for alert timestamps
- ✅ Alert events logged with location, frequency, and movement state
- ✅ Up to 1000 events in RAM (50 clusters × 20 events)
- ✅ Persistent storage in JSON format (~50-100KB)
- ✅ Automatic promotion/demotion based on temporal patterns
- ✅ Memory-efficient with automatic pruning
