// Auto-Lockout Manager - Intelligent location-based false alert learning
// Tracks repeated alerts at specific locations and auto-creates lockout zones
// Thread-safe: All vector operations protected by mutex

#pragma once
#include <Arduino.h>
#include <vector>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "lockout_manager.h"  // For Band enum and LockoutManager

// Single alert event with location and metadata
struct AlertEvent {
  float latitude;
  float longitude;
  float heading;           // GPS heading when alert occurred (0-360, -1 = unknown)
  Band band;
  uint32_t frequency_khz;  // Exact frequency (e.g., 24150 for K-band)
  uint8_t signalStrength;  // 0-9 (from V1)
  uint16_t duration_ms;    // How long signal lasted
  time_t timestamp;        // Unix timestamp
  bool isMoving;           // Speed > threshold when alert occurred
  bool isPersistent;       // Signal lasted >2 seconds (stationary source)
};

// Cluster of similar alerts at same location
struct LearningCluster {
  String name;              // Auto-generated or user-provided
  float centerLat;
  float centerLon;
  float radius_m;           // Calculated from alert spread
  Band band;
  uint32_t frequency_khz;   // Exact frequency (for narrow-band muting)
  float frequency_tolerance_khz;  // Mute range (e.g., ±25 kHz)
  
  std::vector<AlertEvent> events;  // All alerts in this cluster
  
  // Promotion tracking
  int hitCount;             // Number of times alert detected here
  int stoppedHitCount;      // Hits while stopped (faster promotion)
  int movingHitCount;       // Hits while moving (slower promotion)
  time_t firstSeen;
  time_t lastSeen;
  
  // Demotion tracking
  int passWithoutAlertCount;  // Times passed through without alert
  time_t lastPassthrough;
  
  // Interval tracking (JBV1 feature)
  time_t lastCountedHit;      // Last time a hit was counted toward promotion
  time_t lastCountedMiss;     // Last time a miss was counted toward demotion
  
  // Directional unlearn (JBV1 feature)
  float createdHeading;       // GPS heading (degrees) when cluster was created (0-360, -1 = unknown)
  
  // State
  bool isPromoted;          // Has been promoted to lockout
  int promotedLockoutIndex; // Index in LockoutManager (-1 if not promoted)
};

// RAII lock guard for cluster mutex (same pattern as LockoutLock)
class ClusterLock {
public:
  explicit ClusterLock(SemaphoreHandle_t sem) : sem_(sem), locked_(false) {
    if (sem_) {
      locked_ = (xSemaphoreTake(sem_, pdMS_TO_TICKS(100)) == pdTRUE);
    }
  }
  ~ClusterLock() {
    if (sem_ && locked_) {
      xSemaphoreGive(sem_);
    }
  }
  bool ok() const { return locked_; }
private:
  SemaphoreHandle_t sem_;
  bool locked_;
};

class AutoLockoutManager {
private:
  std::vector<LearningCluster> clusters;
  mutable SemaphoreHandle_t clusterMutex;  // Protects clusters vector
  LockoutManager* lockoutManager;  // Pointer to manual lockout manager
  
  // Fixed constants (not user-configurable)
  static constexpr int PROMOTION_STOPPED_HIT_COUNT = 2;  // Stopped: 2 alerts (faster)
  static constexpr int PROMOTION_MOVING_HIT_COUNT = 4;   // Moving: 4 alerts (slower)
  static constexpr int PROMOTION_TIME_WINDOW_DAYS = 2;   // Within 2 days
  static constexpr float CLUSTER_RADIUS_M = 150.0f;      // Alerts within 150m = same location
  static constexpr int DEMOTION_TIME_WINDOW_DAYS = 7;    // Within 7 days
  static constexpr float PASSTHROUGH_RADIUS_M = 200.0f;  // Slightly larger for detection
  static constexpr uint8_t MIN_SIGNAL_STRENGTH = 3;      // Ignore weak signals (< 3)
  static constexpr float DIRECTIONAL_UNLEARN_TOLERANCE_DEG = 90.0f;  // ±90° = same direction
  static constexpr float STOPPED_SPEED_THRESHOLD_MPS = 2.0f;  // < 2 m/s = stopped
  static constexpr size_t MAX_CLUSTERS = 50;             // Max learning clusters
  static constexpr size_t MAX_EVENTS_PER_CLUSTER = 20;   // Max events stored per cluster
  
  // User-configurable settings are read from settingsManager at runtime:
  // - lockoutEnabled: master enable
  // - lockoutKaProtection: never learn Ka
  // - lockoutDirectionalUnlearn: only unlearn in same direction
  // - lockoutFreqToleranceMHz: frequency tolerance (default 8 MHz)
  // - lockoutLearnCount: hits to promote (default 3)
  // - lockoutUnlearnCount: misses to demote auto (default 5)
  // - lockoutManualDeleteCount: misses to demote manual (default 25)
  // - lockoutLearnIntervalHours: hours between counted hits (default 4)
  // - lockoutUnlearnIntervalHours: hours between counted misses (default 4)
  // - lockoutMaxSignalStrength: don't learn >= this (0=disabled)
  // - lockoutMaxDistanceM: max distance to learn (default 600m)
  
  // Helper functions
  int findCluster(float lat, float lon, Band band, uint32_t frequency_khz) const;
  void addEventToCluster(int clusterIdx, const AlertEvent& event);
  void createNewCluster(const AlertEvent& event);
  bool shouldPromoteCluster(const LearningCluster& cluster) const;
  void promoteCluster(int clusterIdx);
  void demoteCluster(int clusterIdx);
  void pruneOldEvents();  // Remove events older than time window
  void pruneOldClusters();  // Remove stale clusters
  String generateClusterName(const LearningCluster& cluster) const;
  
public:
  AutoLockoutManager();
  
  // Set reference to manual lockout manager (for promotion)
  void setLockoutManager(LockoutManager* manager) { lockoutManager = manager; }
  ~AutoLockoutManager();
  
  // Core functionality
  void recordAlert(float lat, float lon, Band band, uint32_t frequency_khz, 
                   uint8_t signalStrength, uint16_t duration_ms, bool isMoving, float heading = -1.0f);
  void recordPassthrough(float lat, float lon, float heading = -1.0f);  // heading: GPS course (0-360, -1 = unknown)
  void update();  // Call periodically to check promotion/demotion
  
  // Storage
  bool loadFromJSON(const char* jsonPath);
  bool saveToJSON(const char* jsonPath);
  
  // SD card backup (survives firmware updates)
  bool backupToSD();
  bool restoreFromSD();
  bool checkAndRestoreFromSD();  // Auto-restore if LittleFS is empty
  
  // Query (thread-safe)
  int getClusterCount() const;
  const LearningCluster* getClusterAtIndex(int idx) const;
  std::vector<int> getClustersNearLocation(float lat, float lon, float radius_m) const;
  
  // Manual control
  void promoteClusterManually(int clusterIdx);
  void deleteCluster(int clusterIdx);
  void clearAll();
  
  // Debug/diagnostics
  void printClusterStats() const;
};
