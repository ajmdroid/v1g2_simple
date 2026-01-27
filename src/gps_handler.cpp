// GPS Handler Implementation
// Supports both Adafruit PA1616S and M10-25Q (via TinyGPSPlus)

#include "gps_handler.h"
#include "config.h"
#include "debug_logger.h"
#include <cmath>

static constexpr bool DEBUG_LOGS = true;  // Set true for verbose GPS logging

#ifdef USE_TINYGPS
// ============================================================================
// TinyGPSPlus Implementation (for M10-25Q and u-blox modules)
// ============================================================================

GPSHandler::GPSHandler() 
  : gpsSerial(Serial2), enabled(false) {
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
}

void GPSHandler::begin() {
  // Enable GPS module via EN pin (LOW = enabled)
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, LOW);
  delay(50);  // Allow GPS to power up
  
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  enabled = true;
  
  // Reset detection state on begin (allows re-enabling)
  moduleDetected = false;
  detectionComplete = false;
  detectionStartMs = millis();
  
  delay(100);
  
  if (DEBUG_LOGS) {
    Serial.println("[GPS] TinyGPSPlus initialized for M10-25Q (NMEA parser)");
    Serial.printf("[GPS] Wiring: TX->GPIO%d, RX->GPIO%d, EN->GPIO%d\n", GPS_TX_PIN, GPS_RX_PIN, GPS_EN_PIN);
  }
}

void GPSHandler::end() {
  if (!enabled) return;
  
  enabled = false;
  gpsSerial.end();
  
  // Clear fix to prevent stale data
  lastFix.valid = false;
  
  if (DEBUG_LOGS) {
    Serial.println("[GPS] Disabled (serial released)");
  }
}

GPSHandler::~GPSHandler() {
  end();
}

void GPSHandler::reset() {
  if (!enabled) {
    Serial.println("[GPS] Reset requested but GPS not enabled");
    return;
  }
  
  Serial.println("[GPS] Power cycling GPS module...");
  
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
  
  Serial.println("[GPS] Reset complete - module re-enabled");
  debugLogger.log(DebugLogCategory::Gps, "Reset complete - searching for satellites");
}

bool GPSHandler::update() {
  // Skip if not enabled or detection already failed
  if (!enabled || (detectionComplete && !moduleDetected)) {
    return false;
  }
  
  // Feed TinyGPSPlus with available serial data
  bool hasData = false;
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c);
    hasData = true;
  }
  
  // Module detection: if we receive any NMEA data, module is present
  if (!detectionComplete) {
    if (hasData) {
      moduleDetected = true;
      detectionComplete = true;
      if (DEBUG_LOGS) {
        Serial.println("[GPS] Module detected");
      }
      debugLogger.log(DebugLogCategory::Gps, "Module detected - waiting for satellite fix");
    } else if (millis() - detectionStartMs > DETECTION_TIMEOUT_MS) {
      detectionComplete = true;
      moduleDetected = false;
      if (DEBUG_LOGS) {
        Serial.println("[GPS] Module NOT detected (timeout) - GPS disabled");
      }
      debugLogger.log(DebugLogCategory::Gps, "Module NOT detected (60s timeout) - GPS disabled");
      return false;
    }
  }
  
  // Check if we have valid location data
  if (gps.location.isValid() && gps.location.age() < 1000) {
    lastFix.latitude = gps.location.lat();
    lastFix.longitude = gps.location.lng();
    lastFix.valid = true;
    lastFix.timestamp_ms = millis();
    
    // Fix quality
    lastFix.hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 999.0;
    lastFix.satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
    
    // Time (UTC)
    if (gps.time.isValid() && gps.date.isValid()) {
      lastFix.hour = gps.time.hour();
      lastFix.minute = gps.time.minute();
      lastFix.seconds = gps.time.second();
      lastFix.year = gps.date.year() - 2000;  // Store as years since 2000
      lastFix.month = gps.date.month();
      lastFix.day = gps.date.day();
      
      // Convert to Unix timestamp
      struct tm timeinfo;
      timeinfo.tm_year = gps.date.year() - 1900;  // tm_year is years since 1900
      timeinfo.tm_mon = gps.date.month() - 1;     // tm_mon is 0-11
      timeinfo.tm_mday = gps.date.day();
      timeinfo.tm_hour = gps.time.hour();
      timeinfo.tm_min = gps.time.minute();
      timeinfo.tm_sec = gps.time.second();
      timeinfo.tm_isdst = 0;  // UTC doesn't have DST
      lastFix.unixTime = mktime(&timeinfo);
    }
    
    // Speed and heading
    lastFix.speed_mps = gps.speed.isValid() ? gps.speed.mps() : 0.0;
    lastFix.heading_deg = gps.course.isValid() ? gps.course.deg() : 0.0;
    
    if (DEBUG_LOGS) {
      Serial.printf("[GPS] Fix: %.6f, %.6f | HDOP: %.1f | Sats: %d | Speed: %.1f m/s\n",
                    lastFix.latitude, lastFix.longitude, 
                    lastFix.hdop, lastFix.satellites, lastFix.speed_mps);
      if (gps.time.isValid() && gps.date.isValid()) {
        Serial.printf("[GPS] Time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                      gps.date.year(), gps.date.month(), gps.date.day(),
                      gps.time.hour(), gps.time.minute(), gps.time.second());
      }
    }
    
    // Log to SD card if GPS category enabled
    if (debugLogger.isEnabledFor(DebugLogCategory::Gps)) {
      debugLogger.logf(DebugLogCategory::Gps, "Fix: %.6f, %.6f | HDOP: %.1f | Sats: %d | Speed: %.1f m/s",
                       lastFix.latitude, lastFix.longitude,
                       lastFix.hdop, lastFix.satellites, lastFix.speed_mps);
    }
    
    return true;
  } else {
    lastFix.valid = false;
    
    // Log search status every 5 seconds using static timer
    static uint32_t lastSearchLog = 0;
    static uint32_t lastSearchLogSD = 0;
    if (DEBUG_LOGS && (millis() - lastSearchLog > 5000)) {
      lastSearchLog = millis();
      Serial.printf("[GPS] Searching... Sats: %d | Chars: %lu | Sentences: %lu | Checksum fail: %lu\n", 
                    gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                    gps.charsProcessed(),
                    gps.sentencesWithFix(),
                    gps.failedChecksum());
    }
    // Log to SD every 30 seconds (less frequent to avoid filling card)
    if (debugLogger.isEnabledFor(DebugLogCategory::Gps) && (millis() - lastSearchLogSD > 30000)) {
      lastSearchLogSD = millis();
      debugLogger.logf(DebugLogCategory::Gps, "Searching... Sats: %d | Chars: %lu | Sentences: %lu",
                       gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                       gps.charsProcessed(),
                       gps.sentencesWithFix());
    }
  }
  
  return false;
}

#else
// ============================================================================
// Adafruit_GPS Implementation (for PA1616S)
// ============================================================================

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
  // This makes the GPS forget all satellite data and start fresh
  GPS.sendCommand("$PMTK104*37");  // Full cold start
  delay(500);  // Give module time to reset
  
  // Configure GPS for optimal lockout performance
  // PA1616S supports 10Hz update rate - use it for smooth geofence detection
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);  // RMC (position) + GGA (fix quality)
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_10HZ);    // 10Hz = 100ms refresh
  GPS.sendCommand(PGCMD_ANTENNA);                 // Enable antenna status messages
  
  delay(100);
  
  if (DEBUG_LOGS) {
    Serial.println("[GPS] Adafruit PA1616S initialized (10Hz, GPS+GLONASS+Galileo) - cold start issued");
    Serial.printf("[GPS] Wiring: TX->GPIO%d, RX->GPIO%d, EN->GPIO%d\n", GPS_TX_PIN, GPS_RX_PIN, GPS_EN_PIN);
  }
}

void GPSHandler::end() {
  if (!enabled) return;
  
  enabled = false;
  gpsSerial.end();
  
  // Power off GPS module via EN pin (HIGH = disabled)
  digitalWrite(GPS_EN_PIN, HIGH);
  
  // Clear fix to prevent stale data
  lastFix.valid = false;
  
  if (DEBUG_LOGS) {
    Serial.println("[GPS] Disabled (serial released, power off)");
  }
}

GPSHandler::~GPSHandler() {
  end();
}

void GPSHandler::reset() {
  if (!enabled) {
    Serial.println("[GPS] Reset requested but GPS not enabled");
    return;
  }
  
  Serial.println("[GPS] Power cycling GPS module...");
  
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
  
  Serial.println("[GPS] Reset complete - cold start issued, searching for satellites");
  debugLogger.log(DebugLogCategory::Gps, "Cold start reset - searching for satellites");
}

bool GPSHandler::update() {
  // Skip if not enabled or detection already failed
  if (!enabled || (detectionComplete && !moduleDetected)) {
    return false;
  }
  
  // Read ALL available GPS data (critical - must drain buffer each call)
  // At 9600 baud with 10Hz output, we get ~100+ chars/sec
  // If we only read one char per update(), serial buffer overflows
  while (gpsSerial.available() > 0) {
    GPS.read();
  }
  
  // Check if we have a complete NMEA sentence
  if (GPS.newNMEAreceived()) {
    // Module detection: if we receive any NMEA sentence, module is present
    if (!detectionComplete) {
      moduleDetected = true;
      detectionComplete = true;
      if (DEBUG_LOGS) {
        Serial.println("[GPS] Module detected");
      }
      debugLogger.log(DebugLogCategory::Gps, "Module detected - waiting for satellite fix");
    }
    
    if (!GPS.parse(GPS.lastNMEA())) {
      return false;  // Parse failed
    }
    
    // Update fix data if GPS has a valid fix
    // Note: GPS.fix should be true when fixquality >= 1, but check both as fallback
    // fixquality: 0=no fix, 1=GPS fix, 2=DGPS fix, 6=estimated
    bool hasGpsFix = GPS.fix || (GPS.fixquality >= 1 && GPS.satellites > 0);
    
    if (hasGpsFix) {
      lastFix.latitude = GPS.latitudeDegrees;
      lastFix.longitude = GPS.longitudeDegrees;
      lastFix.valid = true;
      lastFix.timestamp_ms = millis();
      lastFix.hdop = GPS.HDOP;
      lastFix.satellites = GPS.satellites;
      
      // Extract GPS time (UTC) - more accurate than ESP32 RTC
      lastFix.hour = GPS.hour;
      lastFix.minute = GPS.minute;
      lastFix.seconds = GPS.seconds;
      lastFix.year = GPS.year;
      lastFix.month = GPS.month;
      lastFix.day = GPS.day;
      
      // Convert GPS time to Unix timestamp
      struct tm timeinfo;
      timeinfo.tm_year = GPS.year + 100;  // tm_year is years since 1900, GPS.year is since 2000
      timeinfo.tm_mon = GPS.month - 1;    // tm_mon is 0-11, GPS.month is 1-12
      timeinfo.tm_mday = GPS.day;
      timeinfo.tm_hour = GPS.hour;
      timeinfo.tm_min = GPS.minute;
      timeinfo.tm_sec = GPS.seconds;
      timeinfo.tm_isdst = 0;  // UTC doesn't have DST
      lastFix.unixTime = mktime(&timeinfo);
      
      // Extract speed and heading
      lastFix.speed_mps = GPS.speed * 0.514444f;  // Convert knots to m/s
      lastFix.heading_deg = GPS.angle;
      
      if (DEBUG_LOGS) {
        Serial.printf("[GPS] FIX ACQUIRED: %.6f, %.6f | HDOP: %.1f | Sats: %d | Speed: %.1f m/s\n",
                      lastFix.latitude, lastFix.longitude, 
                      lastFix.hdop, lastFix.satellites, lastFix.speed_mps);
        Serial.printf("[GPS] Time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                      2000 + lastFix.year, lastFix.month, lastFix.day,
                      lastFix.hour, lastFix.minute, lastFix.seconds);
      }
      
      // Log fix to SD card
      debugLogger.logf(DebugLogCategory::Gps, "FIX: %.6f, %.6f | HDOP: %.1f | Sats: %d",
                       lastFix.latitude, lastFix.longitude, lastFix.hdop, lastFix.satellites);
      
      return true;
    } else {
      // GPS has no fix yet
      lastFix.valid = false;
      
      static uint32_t lastSearchLogSD_ada = 0;
      if (DEBUG_LOGS && (millis() % 5000 < 100)) {  // Log every 5 seconds
        Serial.printf("[GPS] Searching for fix... (Sats: %d, FixQual: %d, Lat: %.6f, Lon: %.6f)\n", 
                      (int)GPS.satellites, (int)GPS.fixquality, GPS.latitudeDegrees, GPS.longitudeDegrees);
      }
      // Log to SD every 30 seconds - include fix quality and lat/lon for debugging
      if (debugLogger.isEnabledFor(DebugLogCategory::Gps) && (millis() - lastSearchLogSD_ada > 30000)) {
        lastSearchLogSD_ada = millis();
        debugLogger.logf(DebugLogCategory::Gps, "Searching... Sats: %d, FixQual: %d, Fix: %d, Lat: %.6f, Lon: %.6f", 
                         (int)GPS.satellites, (int)GPS.fixquality, (int)GPS.fix, GPS.latitudeDegrees, GPS.longitudeDegrees);
      }
    }
  }
  
  // Check detection timeout
  if (!detectionComplete && millis() - detectionStartMs > DETECTION_TIMEOUT_MS) {
    detectionComplete = true;
    moduleDetected = false;
    if (DEBUG_LOGS) {
      Serial.println("[GPS] Module NOT detected (timeout) - GPS disabled");
    }
    debugLogger.log(DebugLogCategory::Gps, "Module NOT detected (60s timeout) - GPS disabled");
  }
  
  return false;
}

#endif

// ============================================================================
// Common Implementation (shared by both GPS libraries)
// ============================================================================

bool GPSHandler::isFixStale(uint32_t maxAge_ms) const {
  return (millis() - lastFix.timestamp_ms) > maxAge_ms;
}

float GPSHandler::haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0f;  // Earth radius in meters
  
  float dLat = (lat2 - lat1) * PI / 180.0f;
  float dLon = (lon2 - lon1) * PI / 180.0f;
  
  float a = sin(dLat/2) * sin(dLat/2) +
            cos(lat1 * PI / 180.0f) * cos(lat2 * PI / 180.0f) *
            sin(dLon/2) * sin(dLon/2);
  
  float c = 2 * atan2(sqrt(a), sqrt(1-a));
  
  return R * c;
}
