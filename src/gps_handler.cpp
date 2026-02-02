// GPS Handler Implementation
// Adafruit PA1616S only

#include "gps_handler.h"
#include "config.h"
#include "debug_logger.h"
#include <cmath>

// GPS logging macro - logs to Serial AND debugLogger when GPS category enabled
static constexpr bool GPS_DEBUG_LOGS = false;  // Set true for verbose Serial logging
#define GPS_LOG(...) do { \
    if (GPS_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Gps)) debugLogger.logf(DebugLogCategory::Gps, __VA_ARGS__); \
} while(0)

GPSHandler::GPSHandler()
  : GPS(&Serial2), gpsSerial(Serial2), enabled(false) {
  // Initialize lastFix with zero values
  lastFix.latitude = 0;
  lastFix.longitude = 0;
  lastFix.valid = false;
  lastFix.timestamp_ms = 0;
  lastFix.hdop = 999;
  lastFix.satellites = 0;
  lastFix.hour = 0;
  lastFix.minute = 0;
  lastFix.seconds = 0;
  lastFix.year = 0;
  lastFix.month = 0;
  lastFix.day = 0;
  lastFix.unixTime = 0;
  lastFix.speed_mps = 0;
  lastFix.heading_deg = 0;

  // Module detection state
  moduleDetected = false;
  detectionComplete = false;
  detectionStartMs = 0;

  // Ready state
  gpsReady = false;
  goodFixStartMs = 0;
  badFixStartMs = 0;

  // Heading smoothing
  smoothedHeading = 0.0f;
  smoothedHeadingValid = false;
}

void GPSHandler::begin() {
  // Enable GPS module via EN pin (LOW = enabled)
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, LOW);
  delay(50);  // Allow GPS to power up

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  GPS.begin(GPS_BAUD);
  enabled = true;

  // Reset detection state on begin (allows re-enabling)
  moduleDetected = false;
  detectionComplete = false;
  detectionStartMs = millis();

  delay(100);

  // Force full cold start to clear any corrupted ephemeris data
  GPS.sendCommand("$PMTK104*37");  // Full cold start
  delay(500);  // Give module time to reset

  // Configure GPS for optimal lockout performance
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);  // RMC (position) + GGA (fix quality)
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_10HZ);    // 10Hz = 100ms refresh
  GPS.sendCommand(PGCMD_ANTENNA);                // Enable antenna status messages

  delay(100);

  GPS_LOG("[GPS] Adafruit PA1616S initialized (10Hz, GPS+GLONASS+Galileo) - cold start issued\n");
  GPS_LOG("[GPS] Wiring: TX->GPIO%d, RX->GPIO%d, EN->GPIO%d\n", GPS_TX_PIN, GPS_RX_PIN, GPS_EN_PIN);
}

void GPSHandler::end() {
  if (!enabled) return;

  enabled = false;
  gpsSerial.end();

  // Power off GPS module via EN pin (HIGH = disabled)
  digitalWrite(GPS_EN_PIN, HIGH);

  // Clear fix to prevent stale data
  lastFix.valid = false;

  GPS_LOG("[GPS] Disabled (serial released, power off)\n");
}

GPSHandler::~GPSHandler() {
  end();
}

void GPSHandler::reset() {
  if (!enabled) {
    GPS_LOG("[GPS] Reset requested but GPS not enabled\n");
    return;
  }

  GPS_LOG("[GPS] Power cycling GPS module...\n");

  // Disable GPS module via EN pin (HIGH = disabled for PA1616S / Adafruit)
  digitalWrite(GPS_EN_PIN, HIGH);
  delay(500);  // Give module time to fully power down

  // Clear fix and detection state
  lastFix.valid = false;
  lastFix.latitude = 0;
  lastFix.longitude = 0;
  lastFix.satellites = 0;
  lastFix.hdop = 999;
  moduleDetected = false;
  detectionComplete = false;
  detectionStartMs = millis();

  // Re-enable GPS module (LOW = enabled)
  digitalWrite(GPS_EN_PIN, LOW);
  delay(100);  // Allow GPS to power up

  // Force full cold start to clear any corrupted ephemeris data
  GPS.sendCommand("$PMTK104*37");
  delay(500);  // Give module time to reset

  // Re-configure GPS for PA1616S
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_10HZ);
  GPS.sendCommand(PGCMD_ANTENNA);

  GPS_LOG("[GPS] Reset complete - cold start issued, searching for satellites\n");
  debugLogger.log(DebugLogCategory::Gps, "Cold start reset - searching for satellites");
}

bool GPSHandler::update() {
  // Skip if not enabled or detection already failed
  if (!enabled || (detectionComplete && !moduleDetected)) {
    return false;
  }

  // Read ALL available GPS data (critical - must drain buffer each call)
  while (gpsSerial.available() > 0) {
    GPS.read();
  }

  if (GPS.newNMEAreceived()) {
    // Module detection: if we receive any NMEA sentence, module is present
    if (!detectionComplete) {
      moduleDetected = true;
      detectionComplete = true;
      GPS_LOG("[GPS] Module detected\n");
      debugLogger.log(DebugLogCategory::Gps, "Module detected - waiting for satellite fix");
    }

    if (!GPS.parse(GPS.lastNMEA())) {
      return false;  // Parse failed
    }

    // Update fix data if GPS has a valid fix
    bool hasGpsFix = GPS.fix || (GPS.fixquality >= 1 && GPS.satellites > 0);

    if (hasGpsFix) {
      lastFix.latitude = GPS.latitudeDegrees;
      lastFix.longitude = GPS.longitudeDegrees;
      lastFix.valid = true;
      lastFix.timestamp_ms = millis();
      lastFix.hdop = GPS.HDOP;
      lastFix.satellites = GPS.satellites;

      // Extract GPS time (UTC)
      lastFix.hour = GPS.hour;
      lastFix.minute = GPS.minute;
      lastFix.seconds = GPS.seconds;
      lastFix.year = GPS.year;
      lastFix.month = GPS.month;
      lastFix.day = GPS.day;

      // Convert GPS time to Unix timestamp
      struct tm timeinfo;
      timeinfo.tm_year = GPS.year + 100;  // tm_year is years since 1900, GPS.year is since 2000
      timeinfo.tm_mon = GPS.month - 1;
      timeinfo.tm_mday = GPS.day;
      timeinfo.tm_hour = GPS.hour;
      timeinfo.tm_min = GPS.minute;
      timeinfo.tm_sec = GPS.seconds;
      timeinfo.tm_isdst = 0;
      lastFix.unixTime = mktime(&timeinfo);

      // Sync debug logger time from GPS (periodic re-sync every 6 hours)
      static unsigned long lastTimeSyncMs = 0;
      const unsigned long RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;  // 6 hours
      if (GPS.year > 0 && GPS.month > 0) {
        unsigned long now = millis();
        bool needsSync = (lastTimeSyncMs == 0) || ((now - lastTimeSyncMs) >= RESYNC_INTERVAL_MS);
        if (needsSync) {
          if (debugLogger.hasValidTime()) {
            time_t estimatedTime = debugLogger.getUnixTime();
            int32_t drift = static_cast<int32_t>(lastFix.unixTime - estimatedTime);
            if (abs(drift) > 2) {
              GPS_LOG("[GPS] Time drift: %d seconds before resync\n", drift);
            }
          }
          debugLogger.syncTimeFromGPS(2000 + GPS.year, GPS.month, GPS.day,
                                       GPS.hour, GPS.minute, GPS.seconds);
          lastTimeSyncMs = now;
          GPS_LOG("[GPS] System time synced from GPS (UTC)\n");
        }
      }

      // Extract speed and heading
      lastFix.speed_mps = GPS.speed * 0.514444f;  // Convert knots to m/s
      lastFix.heading_deg = GPS.angle;

      GPS_LOG("[GPS] FIX ACQUIRED: %.6f, %.6f | HDOP: %.1f | Sats: %d | Speed: %.1f m/s\n",
              lastFix.latitude, lastFix.longitude,
              lastFix.hdop, lastFix.satellites, lastFix.speed_mps);

      if (debugLogger.isEnabledFor(DebugLogCategory::Gps)) {
        debugLogger.logf(DebugLogCategory::Gps, "FIX: %.6f, %.6f | HDOP: %.1f | Sats: %d",
                         lastFix.latitude, lastFix.longitude, lastFix.hdop, lastFix.satellites);
      }

      updateReadyState();
      return true;
    } else {
      lastFix.valid = false;
      updateReadyState();

      static uint32_t lastSearchLogSD = 0;
      if (debugLogger.isEnabledFor(DebugLogCategory::Gps) && (millis() - lastSearchLogSD > 30000)) {
        lastSearchLogSD = millis();
        debugLogger.logf(DebugLogCategory::Gps, "Searching... Sats: %d, FixQual: %d, Fix: %d, Lat: %.6f, Lon: %.6f",
                         (int)GPS.satellites, (int)GPS.fixquality, (int)GPS.fix,
                         GPS.latitudeDegrees, GPS.longitudeDegrees);
      }
    }
  }

  if (!detectionComplete && millis() - detectionStartMs > DETECTION_TIMEOUT_MS) {
    detectionComplete = true;
    moduleDetected = false;
    GPS_LOG("[GPS] Module NOT detected (timeout) - GPS disabled\n");
    debugLogger.log(DebugLogCategory::Gps, "Module NOT detected (60s timeout) - GPS disabled");
  }

  return false;
}

void GPSHandler::updateReadyState() {
  unsigned long now = millis();

  bool meetsQualityThreshold = (
    lastFix.valid &&
    lastFix.satellites >= READY_MIN_SATS &&
    lastFix.hdop <= READY_MAX_HDOP &&
    !isFixStale()
  );

  if (meetsQualityThreshold) {
    if (goodFixStartMs == 0) {
      goodFixStartMs = now;
    }
    badFixStartMs = 0;

    if (!gpsReady && (now - goodFixStartMs >= READY_ACQUIRE_MS)) {
      gpsReady = true;
      GPS_LOG("[GPS] Ready for navigation (sats=%d, hdop=%.1f)\n",
              lastFix.satellites, lastFix.hdop);
    }
  } else {
    if (badFixStartMs == 0) {
      badFixStartMs = now;
    }
    goodFixStartMs = 0;

    if (gpsReady && (now - badFixStartMs >= READY_DEGRADE_MS)) {
      gpsReady = false;
      GPS_LOG("[GPS] Not ready (fix degraded)\n");
    }
  }

  if (lastFix.valid && lastFix.speed_mps >= HEADING_MIN_SPEED_MPS) {
    float rawHeading = lastFix.heading_deg;

    if (!smoothedHeadingValid) {
      smoothedHeading = rawHeading;
      smoothedHeadingValid = true;
    } else {
      float delta = rawHeading - smoothedHeading;
      if (delta > 180.0f) delta -= 360.0f;
      if (delta < -180.0f) delta += 360.0f;

      smoothedHeading += HEADING_SMOOTH_ALPHA * delta;

      if (smoothedHeading < 0.0f) smoothedHeading += 360.0f;
      if (smoothedHeading >= 360.0f) smoothedHeading -= 360.0f;
    }
  } else {
    smoothedHeadingValid = false;
  }
}

bool GPSHandler::isFixStale(uint32_t maxAge_ms) const {
  return (millis() - lastFix.timestamp_ms) > maxAge_ms;
}

float GPSHandler::haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0f;

  float dLat = (lat2 - lat1) * PI / 180.0f;
  float dLon = (lon2 - lon1) * PI / 180.0f;

  float a = sin(dLat/2) * sin(dLat/2) +
            cos(lat1 * PI / 180.0f) * cos(lat2 * PI / 180.0f) *
            sin(dLon/2) * sin(dLon/2);

  float c = 2 * atan2(sqrt(a), sqrt(1-a));

  return R * c;
}
