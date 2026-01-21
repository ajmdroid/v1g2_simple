// Lockout Manager - Geofence-based alert muting
// Stores GPS lockout zones and checks if current location should mute alerts

#pragma once
#include <Arduino.h>
#include <vector>
#include "packet_parser.h"  // For Band enum

struct Lockout {
  String name;
  float latitude;
  float longitude;
  float radius_m;
  bool enabled;
  
  // Band-specific muting flags
  bool muteX;
  bool muteK;
  bool muteKa;
  bool muteLaser;
};

class LockoutManager {
private:
  std::vector<Lockout> lockouts;
  
  // Helper: Calculate distance between two points (uses haversine)
  float distanceTo(float lat, float lon, const Lockout& lockout) const;
  
public:
  LockoutManager();
  ~LockoutManager();
  
  // Storage management
  bool loadFromJSON(const char* jsonPath);
  bool saveToJSON(const char* jsonPath);
  
  // SD card backup (survives firmware updates)
  bool backupToSD();
  bool restoreFromSD();
  bool checkAndRestoreFromSD();  // Auto-restore if LittleFS is empty
  
  // Lockout management
  void addLockout(const Lockout& lockout);
  void removeLockout(int index);
  void updateLockout(int index, const Lockout& lockout);
  void clearAll();
  
  // Query functions
  int getLockoutCount() const { return lockouts.size(); }
  const Lockout* getLockoutAtIndex(int idx) const;
  int getNearestLockout(float lat, float lon) const;  // Returns index or -1
  
  // Core functionality: Check if current location should mute alert
  bool shouldMuteAlert(float lat, float lon, Band band) const;
  
  // Get all lockouts that contain the given position
  std::vector<int> getActiveLockouts(float lat, float lon) const;
};
