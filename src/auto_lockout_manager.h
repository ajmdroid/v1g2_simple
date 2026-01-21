// Auto-Lockout Manager - Intelligent location-based false alert learning
// Tracks repeated alerts at specific locations and auto-creates lockout zones

#pragma once
#include <Arduino.h>
#include <vector>
#include <ctime>
#include "lockout_manager.h"  // For Band enum and LockoutManager

// Single alert event with location and metadata
struct AlertEvent {
  float latitude;
  float longitude;
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
  
  // State
  bool isPromoted;          // Has been promoted to lockout
  int promotedLockoutIndex; // Index in LockoutManager (-1 if not promoted)
};

class AutoLockoutManager {
private:
  std::vector<LearningCluster> clusters;
  LockoutManager* lockoutManager;  // Pointer to manual lockout manager
  
  // Thresholds for promotion (alert -> lockout)
  static constexpr int PROMOTION_STOPPED_HIT_COUNT = 2;  // Stopped: 2 alerts (faster)
  static constexpr int PROMOTION_MOVING_HIT_COUNT = 4;   // Moving: 4 alerts (slower)
  static constexpr int PROMOTION_TIME_WINDOW_DAYS = 2;  // Within 2 days
  static constexpr float CLUSTER_RADIUS_M = 150.0f;  // Alerts within 150m = same location
  static constexpr float FREQUENCY_TOLERANCE_KHZ = 25.0f;  // Mute ±25 kHz around exact freq
  
  // Thresholds for demotion (lockout -> removed)
  static constexpr int DEMOTION_PASS_COUNT = 2;      // 2 passes without alert
  static constexpr int DEMOTION_TIME_WINDOW_DAYS = 7;   // Within 7 days
  static constexpr float PASSTHROUGH_RADIUS_M = 200.0f; // Slightly larger for detection
  
  // Signal strength filtering
  static constexpr uint8_t MIN_SIGNAL_STRENGTH = 3;  // Ignore weak signals (< 3)
  
  // Speed detection (require GPS speed from GPSHandler)
  static constexpr float STOPPED_SPEED_THRESHOLD_MPS = 2.0f;  // < 2 m/s = stopped
  
  // Storage limits
  static constexpr size_t MAX_CLUSTERS = 50;         // Max learning clusters
  static constexpr size_t MAX_EVENTS_PER_CLUSTER = 20;  // Max events stored per cluster
  
  // Helper functions
  int findCluster(float lat, float lon, Band band) const;
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
                   uint8_t signalStrength, uint16_t duration_ms, bool isMoving);
  void recordPassthrough(float lat, float lon);  // Called when passing location without alert
  void update();  // Call periodically to check promotion/demotion
  
  // Storage
  bool loadFromJSON(const char* jsonPath);
  bool saveToJSON(const char* jsonPath);
  
  // SD card backup (survives firmware updates)
  bool backupToSD();
  bool restoreFromSD();
  bool checkAndRestoreFromSD();  // Auto-restore if LittleFS is empty
  
  // Query
  int getClusterCount() const { return clusters.size(); }
  const LearningCluster* getClusterAtIndex(int idx) const;
  std::vector<int> getClustersNearLocation(float lat, float lon, float radius_m) const;
  
  // Manual control
  void promoteClusterManually(int clusterIdx);
  void deleteCluster(int clusterIdx);
  void clearAll();
  
  // Debug/diagnostics
  void printClusterStats() const;
};
