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
  
  // GPS pin configuration
  static constexpr int GPS_RX_PIN = 17;  // ESP32 RX <- GPS TX
  static constexpr int GPS_TX_PIN = 18;  // ESP32 TX -> GPS RX
  static constexpr uint32_t GPS_BAUD = 9600;
  
public:
  GPSHandler();
  ~GPSHandler();
  
  void begin();
  void end();    // Disable GPS and release serial (for static allocation pattern)
  bool update();  // Call in main loop - non-blocking, parses available NMEA
  
  // Enable state (for static allocation - avoids heap fragmentation)
  bool isEnabled() const { return enabled; }
  
  // Module detection
  bool isModuleDetected() const { return moduleDetected; }
  bool isDetectionComplete() const { return detectionComplete; }
  
  GPSFix getFix() const { return lastFix; }
  bool hasValidFix() const { return enabled && lastFix.valid && !isFixStale(); }
  bool isFixStale(uint32_t maxAge_ms = 30000) const;
  
  // Time from GPS (more accurate than ESP32 RTC)
  time_t getGPSTime() const { return lastFix.unixTime; }
  bool hasValidTime() const { return lastFix.unixTime > 0; }
  
  // Movement detection
  bool isMoving(float threshold_mps = 2.0f) const { return lastFix.speed_mps > threshold_mps; }
  float getSpeed() const { return lastFix.speed_mps; }
  float getHeading() const { return lastFix.heading_deg; }
  
  // Utility: Calculate distance between two lat/lon points (meters)
  static float haversineDistance(float lat1, float lon1, float lat2, float lon2);
};
