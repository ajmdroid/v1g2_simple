// Lockout Manager - Geofence-based alert muting
// Stores GPS lockout zones and checks if current location should mute alerts
// Thread-safe: All vector operations protected by mutex

#pragma once
#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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

// RAII lock guard for lockout mutex
class LockoutLock {
public:
  explicit LockoutLock(SemaphoreHandle_t sem) : sem_(sem), locked_(false) {
    if (sem_) {
      locked_ = (xSemaphoreTake(sem_, pdMS_TO_TICKS(100)) == pdTRUE);
    }
  }
  ~LockoutLock() {
    if (sem_ && locked_) {
      xSemaphoreGive(sem_);
    }
  }
  bool ok() const { return locked_; }
private:
  SemaphoreHandle_t sem_;
  bool locked_;
};

class LockoutManager {
private:
  std::vector<Lockout> lockouts;
  mutable SemaphoreHandle_t lockoutMutex;  // Protects lockouts vector
  
  // Helper: Calculate distance between two points (uses haversine)
  float distanceTo(float lat, float lon, const Lockout& lockout) const;
  bool isValidLockout(const Lockout& lockout) const;
  bool isDuplicate(const Lockout& lockout, int ignoreIndex = -1) const;
  
public:
  LockoutManager();
  ~LockoutManager();
  
  // Storage management
  bool loadFromJSON(const char* jsonPath);
  bool saveToJSON(const char* jsonPath, bool skipBackup = false);
  
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
  int getLockoutCount() const;
  const Lockout* getLockoutAtIndex(int idx) const;
  int getNearestLockout(float lat, float lon) const;  // Returns index or -1
  
  // Core functionality: Check if current location should mute alert
  bool shouldMuteAlert(float lat, float lon, Band band) const;
  
  // Get all lockouts that contain the given position
  std::vector<int> getActiveLockouts(float lat, float lon) const;
};
