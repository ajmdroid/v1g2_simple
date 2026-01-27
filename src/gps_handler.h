// GPS Handler for Adafruit PA1616S GPS Module (or M10-25Q with TinyGPSPlus)
// Manages GPS fix acquisition and provides location data for geofence lockouts

#pragma once

// Uncomment to use TinyGPSPlus (for M10-25Q or other u-blox modules)
// #define USE_TINYGPS

#ifdef USE_TINYGPS
#include <TinyGPSPlus.h>
#else
#include <Adafruit_GPS.h>
#endif

#include <HardwareSerial.h>
#include <Arduino.h>

struct GPSFix {
  float latitude;
  float longitude;
  bool valid;
  uint32_t timestamp_ms;  // millis() when fix acquired
  float hdop;  // Horizontal dilution of precision
  uint8_t satellites;
  
  // GPS time (UTC)
  uint8_t hour;
  uint8_t minute;
  uint8_t seconds;
  uint8_t year;
  uint8_t month;
  uint8_t day;
  time_t unixTime;  // Unix timestamp from GPS (UTC)
  
  // Movement data
  float speed_mps;  // Speed in meters per second
  float heading_deg;  // Heading in degrees (0-360)
};

class GPSHandler {
private:
#ifdef USE_TINYGPS
  TinyGPSPlus gps;
#else
  Adafruit_GPS GPS;
#endif
  HardwareSerial& gpsSerial;
  GPSFix lastFix;
  
  // Module detection state
  bool moduleDetected;
  bool detectionComplete;
  uint32_t detectionStartMs;
  static constexpr uint32_t DETECTION_TIMEOUT_MS = 60000;  // 60 seconds to detect module
  
  // Enable state (for static allocation pattern)
  bool enabled;
  
  // Ready state: sustained good fix for navigation/alerts
  bool gpsReady;                    // True when GPS is stable enough for alerts
  unsigned long goodFixStartMs;     // When current good fix started
  unsigned long badFixStartMs;      // When fix degraded
  
  // Heading smoothing
  float smoothedHeading;
  bool smoothedHeadingValid;
  
  void updateReadyState();  // Internal: update ready state based on fix quality
  
  // GPS pin configuration (Waveshare ESP32-S3-Touch-LCD-3.49)
  // Note: GPIO 17/18 are used for I2C touch, so we use GPIO 1/2/3 for GPS
  static constexpr int GPS_RX_PIN = 1;   // ESP32 RX <- GPS TX
  static constexpr int GPS_TX_PIN = 2;   // ESP32 TX -> GPS RX
  static constexpr int GPS_EN_PIN = 3;   // GPS Enable (LOW=on, HIGH=off)
  static constexpr uint32_t GPS_BAUD = 9600;
  
  // Ready state thresholds
  static constexpr uint8_t READY_MIN_SATS = 5;
  static constexpr float READY_MAX_HDOP = 2.0f;
  static constexpr unsigned long READY_ACQUIRE_MS = 15000;  // 15s sustained good fix
  static constexpr unsigned long READY_DEGRADE_MS = 5000;   // 5s grace before not-ready
  
  // Heading smoothing
  static constexpr float HEADING_MIN_SPEED_MPS = 1.34f;  // 3 mph minimum
  static constexpr float HEADING_SMOOTH_ALPHA = 0.25f;
  
public:
  GPSHandler();
  ~GPSHandler();
  
  void begin();
  void end();    // Disable GPS and release serial (for static allocation pattern)
  void reset();  // Power cycle GPS module via EN pin to re-acquire fix
  bool update();  // Call in main loop - non-blocking, parses available NMEA
  
  // Enable state (for static allocation - avoids heap fragmentation)
  bool isEnabled() const { return enabled; }
  
  // Module detection
  bool isModuleDetected() const { return moduleDetected; }
  bool isDetectionComplete() const { return detectionComplete; }
  
  GPSFix getFix() const { return lastFix; }
  bool hasValidFix() const { return enabled && lastFix.valid && !isFixStale(); }
  bool isFixStale(uint32_t maxAge_ms = 30000) const;
  
  // Ready for navigation: sustained good fix (use for camera/lockout alerts)
  bool isReadyForNavigation() const { return enabled && gpsReady && hasValidFix(); }
  
  // Time from GPS (more accurate than ESP32 RTC)
  time_t getGPSTime() const { return lastFix.unixTime; }
  bool hasValidTime() const { return lastFix.unixTime > 0; }
  
  // Movement detection
  bool isMoving(float threshold_mps = 2.0f) const { return lastFix.speed_mps > threshold_mps; }
  float getSpeed() const { return lastFix.speed_mps; }
  float getHeading() const { return lastFix.heading_deg; }
  
  // Smoothed heading: returns smoothed heading (0-360) or NaN if invalid/too slow
  float getSmoothedHeading() const { return smoothedHeadingValid ? smoothedHeading : NAN; }
  
  // Utility: Calculate distance between two lat/lon points (meters)
  static float haversineDistance(float lat1, float lon1, float lat2, float lon2);
};
