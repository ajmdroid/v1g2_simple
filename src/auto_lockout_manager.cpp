// Auto-Lockout Manager Implementation
// Intelligent false alert learning with spatial/temporal clustering

#include "auto_lockout_manager.h"
#include "gps_handler.h"
#include "lockout_manager.h"
#include "storage_manager.h"
#include "settings.h"
#include "debug_logger.h"
#include "modules/perf/debug_macros.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include <cmath>
#include <algorithm>

// Ensure the profiles directory exists on the active filesystem
static bool ensureProfilesDir(fs::FS* fs) {
  if (!fs) {
    Serial.println("[AutoLockout] ensureProfilesDir: no filesystem");
    return false;
  }
  if (fs->exists("/v1profiles")) return true;
  bool created = fs->mkdir("/v1profiles");
  if (!created) {
    Serial.println("[AutoLockout] ensureProfilesDir: mkdir /v1profiles FAILED");
  }
  return created;
}

// DEBUG_LOGS defined in debug_macros.h (included above)

// Validate cluster data after parsing (matches LockoutManager::isValidLockout pattern)
static bool isValidCluster(const LearningCluster& cluster) {
  if (!isfinite(cluster.centerLat) || !isfinite(cluster.centerLon) || !isfinite(cluster.radius_m)) {
    return false;
  }
  if (cluster.centerLat < -90.0f || cluster.centerLat > 90.0f) return false;
  if (cluster.centerLon < -180.0f || cluster.centerLon > 180.0f) return false;
  if (cluster.radius_m < 0.0f || cluster.radius_m > 10000.0f) return false;  // Sanity: 0-10km
  if (cluster.hitCount < 0 || cluster.hitCount > 10000) return false;  // Sanity check
  return true;
}

// Lockout logging macro - logs to SD when category enabled
#define LOCKOUT_LOGF(...) do { \
    if (DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Lockout)) debugLogger.logf(DebugLogCategory::Lockout, __VA_ARGS__); \
} while(0)

// Use global instances from main.cpp
extern LockoutManager lockouts;
extern SettingsManager settingsManager;
extern StorageManager storageManager;

AutoLockoutManager::AutoLockoutManager() : clusterMutex(nullptr), lockoutManager(nullptr) {
  clusterMutex = xSemaphoreCreateMutex();
  if (!clusterMutex) {
    Serial.println("[AutoLockout] Failed to create mutex!");
  }
  lastSnapshotMs = millis();
  resetSessionStats();
}

void AutoLockoutManager::setLockoutManager(LockoutManager* manager) {
  lockoutManager = manager;
  
  // Register callback to handle external lockout removals
  // This keeps promotedLockoutIndex values in sync when lockouts are deleted
  // through means other than demoteCluster() (e.g., future API endpoints)
  if (manager) {
    manager->setOnLockoutRemovedCallback([this](int removedIndex) {
      onLockoutRemoved(removedIndex);
    });
  }
}

void AutoLockoutManager::onLockoutRemoved(int removedIndex) {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return;
  
  // Update indices for all promoted clusters that pointed at or after the removed index
  for (auto& c : clusters) {
    if (!c.isPromoted) continue;
    
    if (c.promotedLockoutIndex == removedIndex) {
      // This cluster's promoted lockout was removed externally
      // Mark it as no longer promoted
      c.isPromoted = false;
      c.promotedLockoutIndex = -1;
      if (DEBUG_LOGS) {
        Serial.printf("[AutoLockout] Cluster '%s' lockout removed externally\n", c.name.c_str());
      }
    } else if (c.promotedLockoutIndex > removedIndex) {
      // Adjust index to account for removal
      c.promotedLockoutIndex--;
    }
  }
}

AutoLockoutManager::~AutoLockoutManager() {
  if (clusterMutex) {
    vSemaphoreDelete(clusterMutex);
    clusterMutex = nullptr;
  }
}

int AutoLockoutManager::findCluster(float lat, float lon, Band band, uint32_t frequency_khz) const {
  // Note: Caller must hold clusterMutex
  
  // Get frequency tolerance from settings
  const V1Settings& s = settingsManager.get();
  float freqToleranceKHz = s.lockoutFreqToleranceMHz * 1000.0f;  // Convert MHz to kHz
  
  for (size_t i = 0; i < clusters.size(); i++) {
    // Must match band
    if (clusters[i].band != band) continue;
    
    // Check frequency tolerance (prevents merging different sources at same location)
    // e.g., door opener at 24.150 GHz vs speed sign at 24.125 GHz
    int32_t freqDiff = (int32_t)frequency_khz - (int32_t)clusters[i].frequency_khz;
    if (freqDiff < 0) freqDiff = -freqDiff;  // abs()
    if ((float)freqDiff > freqToleranceKHz) continue;
    
    // Check distance to cluster center
    float dist = GPSHandler::haversineDistance(lat, lon, 
                                                clusters[i].centerLat, 
                                                clusters[i].centerLon);
    
    if (dist <= CLUSTER_RADIUS_M) {
      return i;
    }
  }
  
  return -1;  // No matching cluster
}

void AutoLockoutManager::addEventToCluster(int clusterIdx, const AlertEvent& event) {
  LearningCluster& cluster = clusters[clusterIdx];
  
  // Add event to history (always for location averaging)
  cluster.events.push_back(event);
  
  // Limit event history per cluster (memory constraint)
  if (cluster.events.size() > MAX_EVENTS_PER_CLUSTER) {
    // Remove oldest event
    cluster.events.erase(cluster.events.begin());
  }
  
  // Update lastSeen regardless of interval
  cluster.lastSeen = event.timestamp;
  
  // Get runtime settings for learn interval
  const V1Settings& s = settingsManager.get();
  time_t learnIntervalSec = s.lockoutLearnIntervalHours * 3600;
  
  // JBV1 Learn Interval: Only count hit if enough time has passed since last counted hit
  // This prevents the same alert from being counted multiple times in one pass
  time_t timeSinceLastHit = event.timestamp - cluster.lastCountedHit;
  bool countThisHit = (cluster.lastCountedHit == 0) || (learnIntervalSec == 0) || (timeSinceLastHit >= learnIntervalSec);
  
  if (countThisHit) {
    // Count this toward promotion
    cluster.hitCount++;
    if (event.isMoving) {
      cluster.movingHitCount++;
    } else {
      cluster.stoppedHitCount++;
    }
    cluster.lastCountedHit = event.timestamp;
    
    // Reset miss counter on any counted hit (JBV1 behavior)
    cluster.passWithoutAlertCount = 0;
    
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] Added hit to cluster '%s' (hits: %d [%d stopped/%d moving])\n",
                    cluster.name.c_str(), cluster.hitCount, cluster.stoppedHitCount, cluster.movingHitCount);
    }
  } else {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] Skipped hit to cluster '%s' (interval: %lld sec, need: %lld sec)\n",
                    cluster.name.c_str(), timeSinceLastHit, learnIntervalSec);
    }
  }
  
  // Recalculate cluster center (weighted average of all events)
  float sumLat = 0, sumLon = 0;
  uint32_t sumFreq = 0;
  for (const auto& e : cluster.events) {
    sumLat += e.latitude;
    sumLon += e.longitude;
    sumFreq += e.frequency_khz;
  }
  cluster.centerLat = sumLat / cluster.events.size();
  cluster.centerLon = sumLon / cluster.events.size();
  cluster.frequency_khz = sumFreq / cluster.events.size();  // Average frequency
  
  // Recalculate radius (max distance from center)
  float maxDist = 0;
  for (const auto& e : cluster.events) {
    float dist = GPSHandler::haversineDistance(cluster.centerLat, cluster.centerLon,
                                                e.latitude, e.longitude);
    if (dist > maxDist) maxDist = dist;
  }
  cluster.radius_m = std::max(50.0f, maxDist + 20.0f);  // Min 50m, +20m buffer
}

void AutoLockoutManager::createNewCluster(const AlertEvent& event) {
  // Check storage limit
  if (clusters.size() >= MAX_CLUSTERS) {
    if (DEBUG_LOGS) {
      Serial.println("[AutoLockout] Max clusters reached, pruning oldest");
    }
    pruneOldClusters();
  }
  
  // Get runtime settings
  const V1Settings& s = settingsManager.get();
  
  LearningCluster cluster;
  cluster.centerLat = event.latitude;
  cluster.centerLon = event.longitude;
  cluster.radius_m = 50.0f;  // Initial radius
  cluster.band = event.band;
  cluster.frequency_khz = event.frequency_khz;
  cluster.frequency_tolerance_khz = s.lockoutFreqToleranceMHz * 1000;  // Convert MHz to kHz
  cluster.events.push_back(event);
  cluster.hitCount = 1;
  cluster.stoppedHitCount = event.isMoving ? 0 : 1;
  cluster.movingHitCount = event.isMoving ? 1 : 0;
  cluster.firstSeen = event.timestamp;
  cluster.lastSeen = event.timestamp;
  cluster.passWithoutAlertCount = 0;
  cluster.lastPassthrough = 0;
  cluster.lastCountedHit = event.timestamp;  // First hit counts
  cluster.lastCountedMiss = 0;
  cluster.createdHeading = event.heading;    // Store heading for directional unlearn
  cluster.isPromoted = false;
  cluster.promotedLockoutIndex = -1;
  cluster.name = generateClusterName(cluster);
  
  clusters.push_back(cluster);
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Created cluster '%s' at (%.6f, %.6f) freq: %lu kHz\n",
                  cluster.name.c_str(), cluster.centerLat, cluster.centerLon, cluster.frequency_khz);
  }
}

bool AutoLockoutManager::shouldPromoteCluster(const LearningCluster& cluster) const {
  // Already promoted?
  if (cluster.isPromoted) return false;
  
  // Get runtime settings for learn count
  const V1Settings& s = settingsManager.get();
  // Require at least 2 stopped hits, and more passes when moving (default 4).
  int requiredStoppedHits = std::max<int>(PROMOTION_STOPPED_HIT_COUNT, s.lockoutLearnCount);
  int requiredMovingHits  = std::max<int>(PROMOTION_MOVING_HIT_COUNT, s.lockoutLearnCount);
  
  // Check hit count threshold (different for stopped vs moving)
  bool hasEnoughStoppedHits = cluster.stoppedHitCount >= requiredStoppedHits;
  bool hasEnoughMovingHits  = cluster.movingHitCount  >= requiredMovingHits;
  
  if (!hasEnoughStoppedHits && !hasEnoughMovingHits) return false;
  
  // Check time window (first to last seen within N days)
  time_t timeSpan = cluster.lastSeen - cluster.firstSeen;
  time_t maxTimeSpan = PROMOTION_TIME_WINDOW_DAYS * 24 * 3600;
  
  if (timeSpan > maxTimeSpan) {
    // Hits spread over too long - reset counter
    // (This is handled in pruneOldEvents)
    return false;
  }
  
  // Check that we have multiple distinct times (not all same alert)
  // Count unique days
  std::vector<int> uniqueDays;
  for (const auto& event : cluster.events) {
    int daysSinceEpoch = event.timestamp / (24 * 3600);
    bool found = false;
    for (int day : uniqueDays) {
      if (day == daysSinceEpoch) {
        found = true;
        break;
      }
    }
    if (!found) uniqueDays.push_back(daysSinceEpoch);
  }
  
  // Require alerts on at least 2 different days
  if (uniqueDays.size() < 2) return false;
  
  return true;
}

void AutoLockoutManager::promoteCluster(int clusterIdx) {
  LearningCluster& cluster = clusters[clusterIdx];
  
  // Create lockout zone
  Lockout lockout;
  lockout.name = cluster.name + " (Auto)";
  lockout.latitude = cluster.centerLat;
  lockout.longitude = cluster.centerLon;
  lockout.radius_m = cluster.radius_m;
  lockout.enabled = true;
  
  // Set band-specific muting
  lockout.muteX = (cluster.band == BAND_X);
  lockout.muteK = (cluster.band == BAND_K);
  lockout.muteKa = (cluster.band == BAND_KA);
  lockout.muteLaser = (cluster.band == BAND_LASER);
  
  // Add to lockout manager
  lockouts.addLockout(lockout);
  lockouts.saveToJSON("/v1profiles/lockouts.json");
  
  // Mark cluster as promoted
  cluster.isPromoted = true;
  cluster.promotedLockoutIndex = lockouts.getLockoutCount() - 1;
  cluster.passWithoutAlertCount = 0;  // Reset demotion counter
  
  // Track session stat
  sessionStats.clustersPromoted++;
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] ✓ PROMOTED '%s' to lockout zone\n", cluster.name.c_str());
  }
}

void AutoLockoutManager::demoteCluster(int clusterIdx) {
  LearningCluster& cluster = clusters[clusterIdx];
  
  if (!cluster.isPromoted) return;
  
  // Remove from lockout manager
  if (cluster.promotedLockoutIndex >= 0) {
    lockouts.removeLockout(cluster.promotedLockoutIndex);
    lockouts.saveToJSON("/v1profiles/lockouts.json");
    
    // Update indices for other promoted clusters
    for (auto& c : clusters) {
      if (c.promotedLockoutIndex > cluster.promotedLockoutIndex) {
        c.promotedLockoutIndex--;
      }
    }
  }
  
  // Delete cluster entirely
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] ✗ DEMOTED '%s' (removed lockout)\n", cluster.name.c_str());
  }
  
  clusters.erase(clusters.begin() + clusterIdx);
}

void AutoLockoutManager::pruneOldEvents() {
  time_t now = time(nullptr);
  time_t maxAge = PROMOTION_TIME_WINDOW_DAYS * 24 * 3600;
  
  for (auto& cluster : clusters) {
    // Remove events older than promotion window
    cluster.events.erase(
      std::remove_if(cluster.events.begin(), cluster.events.end(),
                     [now, maxAge](const AlertEvent& e) {
                       return (now - e.timestamp) > maxAge;
                     }),
      cluster.events.end()
    );
    
    // Recalculate hit count
    cluster.hitCount = cluster.events.size();
  }
}

void AutoLockoutManager::pruneOldClusters() {
  time_t now = time(nullptr);
  time_t maxAge = DEMOTION_TIME_WINDOW_DAYS * 24 * 3600;
  
  // Remove non-promoted clusters that haven't been seen recently
  clusters.erase(
    std::remove_if(clusters.begin(), clusters.end(),
                   [now, maxAge](const LearningCluster& c) {
                     return !c.isPromoted && (now - c.lastSeen) > maxAge;
                   }),
    clusters.end()
  );
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Pruned old clusters (now: %d)\n", clusters.size());
  }
}

String AutoLockoutManager::generateClusterName(const LearningCluster& cluster) const {
  // Generate name based on band and location
  String bandName;
  switch (cluster.band) {
    case BAND_X: bandName = "X"; break;
    case BAND_K: bandName = "K"; break;
    case BAND_KA: bandName = "Ka"; break;
    case BAND_LASER: bandName = "Laser"; break;
    default: bandName = "Unknown"; break;
  }
  
  // Use truncated lat/lon for uniqueness
  char nameBuf[32];
  snprintf(nameBuf, sizeof(nameBuf), "%s-%.4f,%.4f", 
           bandName.c_str(), cluster.centerLat, cluster.centerLon);
  
  return String(nameBuf);
}

void AutoLockoutManager::recordAlert(float lat, float lon, Band band, uint32_t frequency_khz, 
                                      uint8_t signalStrength, uint16_t duration_ms, bool isMoving, float heading) {
  const V1Settings& s = settingsManager.get();
  const char* bandStr = (band == BAND_X) ? "X" : (band == BAND_K) ? "K" : 
                        (band == BAND_KA) ? "Ka" : "Laser";
  
  // Track total alerts
  sessionStats.alertsProcessed++;
  
  // Warn once about invalid maxSignalStrength config (must be 0 or > MIN_SIGNAL_STRENGTH)
  static bool warnedInvalidMaxSig = false;
  if (!warnedInvalidMaxSig && s.lockoutMaxSignalStrength > 0 && s.lockoutMaxSignalStrength <= MIN_SIGNAL_STRENGTH) {
    Serial.printf("[AutoLockout] WARNING: lockoutMaxSignalStrength=%d <= MIN_SIGNAL_STRENGTH=%d, no signals can be learned! Set to 0 (disabled) or > %d\n",
                  s.lockoutMaxSignalStrength, MIN_SIGNAL_STRENGTH, MIN_SIGNAL_STRENGTH);
    warnedInvalidMaxSig = true;
  }
  
  // Check master enable
  if (!s.lockoutEnabled) {
    LOCKOUT_LOGF("[AutoLockout] %s %.3fMHz str=%d -> SKIP (lockout disabled)\n",
                  bandStr, frequency_khz/1000.0f, signalStrength);
    return;
  }
  
  // Ka band protection (user-configurable)
  if (s.lockoutKaProtection && band == BAND_KA) {
    sessionStats.alertsSkippedKa++;
    LOCKOUT_LOGF("[AutoLockout] %s %.3fMHz str=%d -> SKIP (Ka protection)\n",
                  bandStr, frequency_khz/1000.0f, signalStrength);
    return;
  }
  
  // Filter weak signals (likely far away or irrelevant)
  if (signalStrength < MIN_SIGNAL_STRENGTH) {
    sessionStats.alertsSkippedWeak++;
    LOCKOUT_LOGF("[AutoLockout] %s %.3fMHz str=%d -> SKIP (weak signal < %d)\n", 
                  bandStr, frequency_khz/1000.0f, signalStrength, MIN_SIGNAL_STRENGTH);
    return;
  }
  
  // Filter strong signals (user-configurable, 0 = disabled)
  // Note: maxSig must be > MIN_SIGNAL_STRENGTH to have any valid learning range
  uint8_t maxSig = s.lockoutMaxSignalStrength;
  if (maxSig > 0 && maxSig > MIN_SIGNAL_STRENGTH && signalStrength >= maxSig) {
    LOCKOUT_LOGF("[AutoLockout] %s %.3fMHz str=%d -> SKIP (strong signal >= %d)\n", 
                  bandStr, frequency_khz/1000.0f, signalStrength, maxSig);
    return;
  }
  
  // Create alert event
  AlertEvent event;
  event.latitude = lat;
  event.longitude = lon;
  event.heading = heading;  // Store heading for directional unlearn
  event.band = band;
  event.frequency_khz = frequency_khz;
  event.signalStrength = signalStrength;
  event.duration_ms = duration_ms;
  event.timestamp = time(nullptr);  // Will use GPS time if available
  event.isMoving = isMoving;
  event.isPersistent = (duration_ms > 2000);  // >2 seconds = stationary source
  
  if (DEBUG_LOGS) {
    const char* bandStr = (band == BAND_X) ? "X" : (band == BAND_K) ? "K" : 
                          (band == BAND_KA) ? "Ka" : "Laser";
    Serial.printf("[AutoLockout] Recording alert: %s %.3fMHz @ (%.6f,%.6f) heading=%.0f strength=%d %s\n",
                  bandStr, frequency_khz/1000.0f, lat, lon, heading, signalStrength,
                  isMoving ? "moving" : "stopped");
  }
  
  {
    // Lock for vector access
    ClusterLock lock(clusterMutex);
    if (!lock.ok()) {
      LOCKOUT_LOGF("[AutoLockout] Failed to acquire mutex for recordAlert\n");
      return;
    }
    
    // Find or create cluster (now includes frequency matching)
    int clusterIdx = findCluster(lat, lon, band, frequency_khz);
    
    if (clusterIdx >= 0) {
      // Add to existing cluster
      addEventToCluster(clusterIdx, event);
      LearningCluster& c = clusters[clusterIdx];
      sessionStats.clusterHits++;
      LOCKOUT_LOGF("[AutoLockout] %s %.3fMHz str=%d -> CLUSTER #%d (hits=%d)\n",
                    bandStr, frequency_khz/1000.0f, signalStrength, clusterIdx, (int)c.hitCount);
    } else {
      // Create new cluster
      createNewCluster(event);
      sessionStats.clustersCreated++;
      LOCKOUT_LOGF("[AutoLockout] %s %.3fMHz str=%d -> NEW CLUSTER @ (%.6f,%.6f)\n",
                    bandStr, frequency_khz/1000.0f, signalStrength, lat, lon);
    }
  }
  
  // Log hit (after releasing mutex)
  appendLogHit(event);
}

// Helper: Calculate angular difference between two headings (0-180 degrees)
static float headingDifference(float h1, float h2) {
  if (h1 < 0 || h2 < 0) return 0.0f;  // Unknown heading = no check
  float diff = fabs(h1 - h2);
  if (diff > 180.0f) diff = 360.0f - diff;
  return diff;
}

void AutoLockoutManager::recordPassthrough(float lat, float lon, float heading, time_t tsOverride) {
  time_t now = time(nullptr);
  if (tsOverride != 0) now = tsOverride;
  
  // Get runtime settings
  const V1Settings& s = settingsManager.get();
  time_t unlearnIntervalSec = s.lockoutUnlearnIntervalHours * 3600;
  
  // Lock for vector access
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) {
    LOCKOUT_LOGF("[AutoLockout] Failed to acquire mutex for recordPassthrough\n");
    return;
  }
  
  // Find all promoted clusters near this location
  for (auto& cluster : clusters) {
    if (!cluster.isPromoted) continue;
    
    float dist = GPSHandler::haversineDistance(lat, lon, 
                                                cluster.centerLat, 
                                                cluster.centerLon);
    
    // Passed through lockout zone without alert?
    if (dist <= PASSTHROUGH_RADIUS_M) {
      // JBV1 Directional Unlearn: Only count miss if traveling in same direction as when created
      if (s.lockoutDirectionalUnlearn && cluster.createdHeading >= 0 && heading >= 0) {
        float hdgDiff = headingDifference(heading, cluster.createdHeading);
        if (hdgDiff > DIRECTIONAL_UNLEARN_TOLERANCE_DEG) {
          if (DEBUG_LOGS) {
            Serial.printf("[AutoLockout] Skipped miss for '%s' (heading: %.0f° vs created: %.0f°, diff: %.0f° > %.0f°)\n",
                          cluster.name.c_str(), heading, cluster.createdHeading, hdgDiff, DIRECTIONAL_UNLEARN_TOLERANCE_DEG);
          }
          continue;  // Wrong direction, don't count as miss
        }
      }
      
      // JBV1 Unlearn Interval: Only count miss if enough time has passed
      time_t timeSinceLastMiss = now - cluster.lastCountedMiss;
      bool countThisMiss = (cluster.lastCountedMiss == 0) || (unlearnIntervalSec == 0) || (timeSinceLastMiss >= unlearnIntervalSec);
      
      if (countThisMiss) {
        cluster.passWithoutAlertCount++;
        cluster.lastPassthrough = now;
        cluster.lastCountedMiss = now;
        
        if (DEBUG_LOGS) {
          Serial.printf("[AutoLockout] Passthrough '%s' without alert (count: %d)\n",
                        cluster.name.c_str(), cluster.passWithoutAlertCount);
        }
        appendLogMiss(lat, lon, heading, now);
      } else {
        if (DEBUG_LOGS) {
          Serial.printf("[AutoLockout] Skipped miss for '%s' (interval: %lld sec, need: %lld sec)\n",
                        cluster.name.c_str(), timeSinceLastMiss, unlearnIntervalSec);
        }
      }
    }
  }
}

void AutoLockoutManager::update() {
  // Lock for vector access
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) {
    LOCKOUT_LOGF("[AutoLockout] Failed to acquire mutex for update\n");
    return;
  }
  
  // Prune old data
  pruneOldEvents();
  pruneOldClusters();
  
  // Check for promotions
  for (size_t i = 0; i < clusters.size(); i++) {
    if (shouldPromoteCluster(clusters[i])) {
      promoteCluster(i);
    }
  }
  
  // Get runtime settings for demotion
  const V1Settings& s = settingsManager.get();
  int demotionCount = s.lockoutUnlearnCount;
  
  // Check for demotions
  time_t now = time(nullptr);
  time_t demotionWindow = DEMOTION_TIME_WINDOW_DAYS * 24 * 3600;
  
  for (int i = clusters.size() - 1; i >= 0; i--) {
    const auto& cluster = clusters[i];
    
    if (!cluster.isPromoted) continue;
    
    // Check demotion criteria
    bool shouldDemote = false;
    
    // Criterion 1: Passed through N times without alert (N from runtime settings)
    if (cluster.passWithoutAlertCount >= demotionCount) {
      // Check if these passes were recent (within demotion window)
      if ((now - cluster.lastPassthrough) <= demotionWindow) {
        shouldDemote = true;
      }
    }
    
    // Criterion 2: No alerts seen in a long time (stale lockout)
    time_t stalePeriod = 30 * 24 * 3600;  // 30 days
    if ((now - cluster.lastSeen) > stalePeriod) {
      shouldDemote = true;
    }
    
    if (shouldDemote) {
      demoteCluster(i);
    }
  }
}

// Helper: Try to load clusters from a specific filesystem
// Returns true if successfully loaded, populates clusters vector
// Caller must NOT hold clusterMutex - this function acquires it
bool AutoLockoutManager::tryLoadFromFS(fs::FS* fs, const char* jsonPath) {
  if (!fs || !fs->exists(jsonPath)) return false;
  
  File file = fs->open(jsonPath, "r");
  if (!file) return false;
  
  // Bound file size to prevent heap spikes from corrupted files
  // Max reasonable size: 50 clusters × ~1.5KB each = ~75KB, use 100KB limit
  constexpr size_t MAX_SNAPSHOT_SIZE = 100 * 1024;
  size_t fileSize = file.size();
  if (fileSize > MAX_SNAPSHOT_SIZE) {
    Serial.printf("[AutoLockout] WARNING: Snapshot file too large (%u bytes > %u max), skipping\n", 
                  (unsigned)fileSize, (unsigned)MAX_SNAPSHOT_SIZE);
    file.close();
    return false;
  }
  
  // Heap-allocate JsonDocument to avoid large stack frames
  std::unique_ptr<JsonDocument> doc(new JsonDocument());
  DeserializationError error = deserializeJson(*doc, file);
  file.close();
  
  if (error) {
    Serial.printf("[AutoLockout] JSON parse error: %s\n", error.c_str());
    return false;
  }
  
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return false;
  
  clusters.clear();
  
  JsonArray clusterArray = (*doc)["clusters"].as<JsonArray>();
  for (JsonObject obj : clusterArray) {
    LearningCluster cluster;
    cluster.name = obj["name"].as<String>();
    cluster.centerLat = obj["centerLat"].as<float>();
    cluster.centerLon = obj["centerLon"].as<float>();
    cluster.radius_m = obj["radius_m"].as<float>();
    cluster.band = static_cast<Band>(obj["band"].as<int>());
    cluster.frequency_khz = obj["frequency_khz"] | 0;
    cluster.frequency_tolerance_khz = obj["frequency_tolerance_khz"] | 8000;  // Default 8 MHz
    cluster.hitCount = obj["hitCount"].as<int>();
    cluster.stoppedHitCount = obj["stoppedHitCount"] | 0;
    cluster.movingHitCount = obj["movingHitCount"] | 0;
    cluster.firstSeen = obj["firstSeen"].as<time_t>();
    cluster.lastSeen = obj["lastSeen"].as<time_t>();
    cluster.passWithoutAlertCount = obj["passWithoutAlertCount"].as<int>();
    cluster.lastPassthrough = obj["lastPassthrough"].as<time_t>();
    cluster.lastCountedHit = obj["lastCountedHit"] | cluster.lastSeen;  // Default to lastSeen for old data
    cluster.lastCountedMiss = obj["lastCountedMiss"] | cluster.lastPassthrough;  // Default to lastPassthrough
    cluster.createdHeading = obj["createdHeading"] | -1.0f;  // Default to unknown
    cluster.isPromoted = obj["isPromoted"].as<bool>();
    cluster.promotedLockoutIndex = obj["promotedLockoutIndex"].as<int>();
    
    // Load events (if present)
    JsonArray eventsArray = obj["events"].as<JsonArray>();
    for (JsonObject eventObj : eventsArray) {
      AlertEvent event;
      event.latitude = eventObj["lat"].as<float>();
      event.longitude = eventObj["lon"].as<float>();
      event.heading = eventObj["heading"] | -1.0f;  // Default to unknown
      event.band = static_cast<Band>(eventObj["band"].as<int>());
      event.signalStrength = eventObj["signal"].as<uint8_t>();
      event.timestamp = eventObj["time"].as<time_t>();
      event.isMoving = eventObj["moving"].as<bool>();
      
      cluster.events.push_back(event);
    }
    
    // Validate cluster data before adding (skip corrupted entries)
    if (!isValidCluster(cluster)) {
      Serial.printf("[AutoLockout] WARNING: Skipping invalid cluster '%s' (lat=%.2f, lon=%.2f)\n",
                    cluster.name.c_str(), cluster.centerLat, cluster.centerLon);
      continue;
    }
    
    clusters.push_back(cluster);
  }
  
  return true;
}

bool AutoLockoutManager::loadFromJSON(const char* jsonPath) {
  bool loadedSnapshot = false;
  bool replayed = false;
  
  // Try primary filesystem first, then secondary LittleFS as fallback
  fs::FS* primaryFs = storageManager.getFilesystem();
  fs::FS* secondaryFs = storageManager.getLittleFS();
  
  loadedSnapshot = tryLoadFromFS(primaryFs, jsonPath);
  if (!loadedSnapshot && secondaryFs && secondaryFs != primaryFs) {
    loadedSnapshot = tryLoadFromFS(secondaryFs, jsonPath);
  }
  
  // Try SD restore if LittleFS missing/corrupt
  if (!loadedSnapshot) {
    checkAndRestoreFromSD();
  }

  // Replay any newer log entries
  replayed = replayLog();
  relinkPromotedLockouts();

  SerialLog.printf("[AutoLockout] Load complete: clusters=%d snapshot=%s replay=%s\n",
                   getClusterCount(),
                   loadedSnapshot ? "yes" : "no",
                   replayed ? "yes" : "no");

  return loadedSnapshot || !clusters.empty();
}

bool AutoLockoutManager::saveToJSON(const char* jsonPath) {
  JsonDocument doc;
  JsonArray clusterArray = doc["clusters"].to<JsonArray>();
  
  // Lock for vector read access
  {
    ClusterLock lock(clusterMutex);
    if (!lock.ok()) {
      Serial.println("[AutoLockout] Failed to acquire mutex for save");
      return false;
    }
    
    for (const auto& cluster : clusters) {
      JsonObject obj = clusterArray.add<JsonObject>();
      obj["name"] = cluster.name;
      obj["centerLat"] = cluster.centerLat;
      obj["centerLon"] = cluster.centerLon;
      obj["radius_m"] = cluster.radius_m;
      obj["band"] = static_cast<int>(cluster.band);
      obj["frequency_khz"] = cluster.frequency_khz;
      obj["frequency_tolerance_khz"] = cluster.frequency_tolerance_khz;
      obj["hitCount"] = cluster.hitCount;
      obj["stoppedHitCount"] = cluster.stoppedHitCount;
      obj["movingHitCount"] = cluster.movingHitCount;
      obj["firstSeen"] = cluster.firstSeen;
    obj["lastSeen"] = cluster.lastSeen;
    obj["passWithoutAlertCount"] = cluster.passWithoutAlertCount;
    obj["lastPassthrough"] = cluster.lastPassthrough;
    obj["lastCountedHit"] = cluster.lastCountedHit;
    obj["lastCountedMiss"] = cluster.lastCountedMiss;
    obj["createdHeading"] = cluster.createdHeading;
    obj["isPromoted"] = cluster.isPromoted;
    obj["promotedLockoutIndex"] = cluster.promotedLockoutIndex;
    
    // Save recent events (last 5 for debugging)
    JsonArray eventsArray = obj["events"].to<JsonArray>();
    size_t startIdx = cluster.events.size() > 5 ? cluster.events.size() - 5 : 0;
    for (size_t i = startIdx; i < cluster.events.size(); i++) {
      const auto& event = cluster.events[i];
      JsonObject eventObj = eventsArray.add<JsonObject>();
      eventObj["lat"] = event.latitude;
      eventObj["lon"] = event.longitude;
      eventObj["heading"] = event.heading;
      eventObj["band"] = static_cast<int>(event.band);
      eventObj["signal"] = event.signalStrength;
      eventObj["time"] = event.timestamp;
      eventObj["moving"] = event.isMoving;
    }
  }
  } // End of lock scope
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) {
    Serial.println("[AutoLockout] No filesystem available for save");
    return false;
  }
  
  // Ensure directory exists - if it fails, don't try to write
  if (!ensureProfilesDir(fs)) {
    Serial.printf("[AutoLockout] Cannot save: /v1profiles directory unavailable (SD=%s)\n",
                  storageManager.isSDCard() ? "yes" : "no");
    return false;
  }
  
  // Atomic write: write to temp file then rename (prevents corruption on power loss)
  bool ok = StorageManager::writeJsonFileAtomic(*fs, jsonPath, doc);

  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Saved clusters (%d bytes)%s\n", 
                  ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  }
  
  if (!ok) {
    return false;
  }
  
  // Secondary backup to LittleFS when SD is primary
  fs::FS* lfs = storageManager.getLittleFS();
  if (lfs && lfs != fs) {
    ensureProfilesDir(lfs);
    if (!StorageManager::writeJsonFileAtomic(*lfs, jsonPath, doc)) {
      Serial.println("[AutoLockout] WARNING: LittleFS mirror write failed (SD primary OK)");
    }
  }
  
  // Auto-backup to SD card if primary is LittleFS
  if (!storageManager.isSDCard()) {
    backupToSD();
  }
  
  return true;
}

int AutoLockoutManager::getClusterCount() const {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return 0;
  return clusters.size();
}

const LearningCluster* AutoLockoutManager::getClusterAtIndex(int idx) const {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return nullptr;
  
  if (idx >= 0 && idx < (int)clusters.size()) {
    return &clusters[idx];
  }
  return nullptr;
}

std::vector<int> AutoLockoutManager::getClustersNearLocation(float lat, float lon, 
                                                              float radius_m) const {
  std::vector<int> nearClusters;
  
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return nearClusters;
  
  for (size_t i = 0; i < clusters.size(); i++) {
    float dist = GPSHandler::haversineDistance(lat, lon,
                                                clusters[i].centerLat,
                                                clusters[i].centerLon);
    if (dist <= radius_m) {
      nearClusters.push_back(i);
    }
  }
  
  return nearClusters;
}

void AutoLockoutManager::promoteClusterManually(int clusterIdx) {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return;
  
  if (clusterIdx >= 0 && clusterIdx < (int)clusters.size()) {
    promoteCluster(clusterIdx);
    // Note: saveToJSON acquires its own lock, release this one first
  }
  // Lock released here, safe to call saveToJSON
  saveToJSON("/v1simple/auto_lockouts.json");
}

void AutoLockoutManager::deleteCluster(int clusterIdx) {
  {
    ClusterLock lock(clusterMutex);
    if (!lock.ok()) return;
    
    if (clusterIdx >= 0 && clusterIdx < (int)clusters.size()) {
      if (clusters[clusterIdx].isPromoted) {
        demoteCluster(clusterIdx);
      } else {
        clusters.erase(clusters.begin() + clusterIdx);
      }
    }
  }
  saveToJSON("/v1simple/auto_lockouts.json");
}

void AutoLockoutManager::clearAll() {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return;
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Cleared all %d clusters\n", clusters.size());
  }
  clusters.clear();
}

void AutoLockoutManager::printClusterStats() const {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) {
    Serial.println("[AutoLockout] Failed to acquire mutex for printClusterStats");
    return;
  }
  
  Serial.println("\n=== Auto-Lockout Learning Clusters ===");
  
  for (size_t i = 0; i < clusters.size(); i++) {
    const auto& c = clusters[i];
    Serial.printf("[%d] %s | Hits: %d | Promoted: %s | Passes: %d\n",
                  i, c.name.c_str(), c.hitCount,
                  c.isPromoted ? "YES" : "no",
                  c.passWithoutAlertCount);
    Serial.printf("    Location: (%.6f, %.6f) ± %.0fm\n",
                  c.centerLat, c.centerLon, c.radius_m);
    
    time_t now = time(nullptr);
    int daysSinceLastSeen = (now - c.lastSeen) / (24 * 3600);
    Serial.printf("    Last seen: %d days ago\n", daysSinceLastSeen);
  }
  
  Serial.println("======================================\n");
}

// ============================================================================
// SD Card Backup/Restore (survives firmware updates)
// ============================================================================

bool AutoLockoutManager::backupToSD() {
  if (!storageManager.isReady() || !storageManager.isSDCard()) {
    if (DEBUG_LOGS) {
      Serial.println("[AutoLockout] SD card not available for backup");
    }
    return false;
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) return false;
  
  JsonDocument doc;
  doc["_type"] = "v1simple_auto_lockouts_backup";
  doc["_version"] = 1;
  doc["timestamp"] = millis();
  
  JsonArray clusterArray = doc["clusters"].to<JsonArray>();
  
  // Lock for vector read access
  {
    ClusterLock lock(clusterMutex);
    if (!lock.ok()) {
      Serial.println("[AutoLockout] Failed to acquire mutex for SD backup");
      return false;
    }
    
    for (const auto& cluster : clusters) {
      JsonObject obj = clusterArray.add<JsonObject>();
      obj["name"] = cluster.name;
      obj["centerLat"] = cluster.centerLat;
      obj["centerLon"] = cluster.centerLon;
      obj["radius_m"] = cluster.radius_m;
      obj["band"] = static_cast<int>(cluster.band);
      obj["frequency_khz"] = cluster.frequency_khz;
      obj["frequency_tolerance_khz"] = cluster.frequency_tolerance_khz;
      obj["hitCount"] = cluster.hitCount;
      obj["stoppedHitCount"] = cluster.stoppedHitCount;
      obj["movingHitCount"] = cluster.movingHitCount;
      obj["firstSeen"] = static_cast<long>(cluster.firstSeen);
      obj["lastSeen"] = static_cast<long>(cluster.lastSeen);
      obj["passWithoutAlertCount"] = cluster.passWithoutAlertCount;
      obj["lastPassthrough"] = static_cast<long>(cluster.lastPassthrough);
      obj["lastCountedHit"] = static_cast<long>(cluster.lastCountedHit);
      obj["lastCountedMiss"] = static_cast<long>(cluster.lastCountedMiss);
      obj["createdHeading"] = cluster.createdHeading;
      obj["isPromoted"] = cluster.isPromoted;
      obj["promotedLockoutIndex"] = cluster.promotedLockoutIndex;
      
      // Save last 5 events (most recent) to reduce SD write size
      JsonArray events = obj["events"].to<JsonArray>();
      int startIdx = cluster.events.size() > 5 ? cluster.events.size() - 5 : 0;
      for (size_t i = startIdx; i < cluster.events.size(); i++) {
        const auto& ev = cluster.events[i];
        JsonObject evObj = events.add<JsonObject>();
        evObj["lat"] = ev.latitude;
        evObj["lon"] = ev.longitude;
        evObj["heading"] = ev.heading;
        evObj["band"] = static_cast<int>(ev.band);
        evObj["freq"] = ev.frequency_khz;
        evObj["signal"] = ev.signalStrength;
        evObj["duration"] = ev.duration_ms;
        evObj["time"] = static_cast<long>(ev.timestamp);
        evObj["moving"] = ev.isMoving;
        evObj["persistent"] = ev.isPersistent;
      }
    }
  } // Lock released
  
  // Atomic write to SD
  bool ok = StorageManager::writeJsonFileAtomic(*fs, "/v1simple_auto_lockouts.json", doc);
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Backed up clusters to SD (%d bytes)%s\n", 
                  ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  }
  
  return ok;
}

bool AutoLockoutManager::restoreFromSD() {
  if (!storageManager.isReady() || !storageManager.isSDCard()) {
    return false;
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) return false;
  
  if (!fs->exists("/v1simple_auto_lockouts.json")) {
    return false;
  }
  
  File file = fs->open("/v1simple_auto_lockouts.json", "r");
  if (!file) {
    return false;
  }
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] SD backup parse error: %s\n", error.c_str());
    }
    return false;
  }
  
  // Verify backup format
  if (doc["_type"] != "v1simple_auto_lockouts_backup") {
    if (DEBUG_LOGS) {
      Serial.println("[AutoLockout] Invalid SD backup format");
    }
    return false;
  }
  
  // Lock for vector modifications
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) {
    Serial.println("[AutoLockout] Failed to acquire mutex for SD restore");
    return false;
  }
  
  // Clear and restore
  clusters.clear();
  
  JsonArray clusterArray = doc["clusters"].as<JsonArray>();
  for (JsonObject obj : clusterArray) {
    LearningCluster cluster;
    cluster.name = obj["name"].as<String>();
    cluster.centerLat = obj["centerLat"].as<float>();
    cluster.centerLon = obj["centerLon"].as<float>();
    cluster.radius_m = obj["radius_m"].as<float>();
    cluster.band = static_cast<Band>(obj["band"].as<int>());
    cluster.frequency_khz = obj["frequency_khz"].as<uint32_t>();
    cluster.frequency_tolerance_khz = obj["frequency_tolerance_khz"] | 25.0f;
    cluster.hitCount = obj["hitCount"].as<int>();
    cluster.stoppedHitCount = obj["stoppedHitCount"] | 0;
    cluster.movingHitCount = obj["movingHitCount"] | 0;
    cluster.firstSeen = obj["firstSeen"].as<long>();
    cluster.lastSeen = obj["lastSeen"].as<long>();
    cluster.passWithoutAlertCount = obj["passWithoutAlertCount"] | 0;
    cluster.lastPassthrough = obj["lastPassthrough"] | 0L;
    cluster.lastCountedHit = obj["lastCountedHit"] | cluster.lastSeen;  // Default to lastSeen
    cluster.lastCountedMiss = obj["lastCountedMiss"] | cluster.lastPassthrough;  // Default to lastPassthrough
    cluster.createdHeading = obj["createdHeading"] | -1.0f;  // Default to unknown
    cluster.isPromoted = obj["isPromoted"] | false;
    cluster.promotedLockoutIndex = obj["promotedLockoutIndex"] | -1;
    
    // Restore events
    JsonArray events = obj["events"].as<JsonArray>();
    for (JsonObject evObj : events) {
      AlertEvent ev;
      ev.latitude = evObj["lat"].as<float>();
      ev.longitude = evObj["lon"].as<float>();
      ev.heading = evObj["heading"] | -1.0f;  // Default to unknown
      ev.band = static_cast<Band>(evObj["band"].as<int>());
      ev.frequency_khz = evObj["freq"].as<uint32_t>();
      ev.signalStrength = evObj["signal"].as<uint8_t>();
      ev.duration_ms = evObj["duration"].as<uint16_t>();
      ev.timestamp = evObj["time"].as<long>();
      ev.isMoving = evObj["moving"] | false;
      ev.isPersistent = evObj["persistent"] | false;
      cluster.events.push_back(ev);
    }
    
    clusters.push_back(cluster);
  }
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Restored %d clusters from SD backup\n", clusters.size());
  }
  
  // Save to LittleFS
  saveToJSON("/v1simple/auto_lockouts.json");
  
  return true;
}

bool AutoLockoutManager::checkAndRestoreFromSD() {
  // Check if LittleFS is empty and SD backup exists
  if (clusters.empty() && storageManager.isSDCard()) {
    if (DEBUG_LOGS) {
      Serial.println("[AutoLockout] LittleFS empty, checking for SD backup...");
    }
    return restoreFromSD();
  }
  return false;
}

bool AutoLockoutManager::rebuildFromLog() {
  return replayLog();
}

// ============================================================================  
// Crash-safe logging and replay  
// ============================================================================

fs::FS* AutoLockoutManager::logFs() const {
  if (storageManager.isReady() && storageManager.isSDCard()) return storageManager.getFilesystem();
  return &LittleFS;
}

const char* AutoLockoutManager::logPath() const {
  if (storageManager.isReady() && storageManager.isSDCard()) return "/v1simple_auto_lockouts.log";
  return "/auto_lockouts.log";
}

void AutoLockoutManager::ensureLogExists() {
  fs::FS* fs = logFs();
  if (!fs) return;
  const char* path = logPath();
  if (!fs->exists(path)) {
    File f = fs->open(path, "w");
    if (f) f.close();
  }
}

// Synchronous write - used directly when async mode is off, or by writer task
void AutoLockoutManager::appendLogRecordSync(const char* line) {
  fs::FS* fs = logFs();
  if (!fs) return;
  const char* path = logPath();
  File f = fs->open(path, "a");
  if (!f) return;
  f.print(line);
  f.print('\n');
  f.close();
  noteLogWrite();
}

void AutoLockoutManager::appendLogRecord(const JsonDocument& doc) {
  if (replayingLog) return;
  
  // Serialize to buffer
  char buf[LOCKOUT_LOG_LINE_SIZE];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len == 0 || len >= sizeof(buf)) {
    // Serialization failed or truncated
    return;
  }
  
  // If async mode enabled and queue exists, queue for background write
  if (asyncLogMode && logWriteQueue) {
    // Non-blocking send - if queue full, drop this entry (rare, only during burst)
    if (xQueueSend(logWriteQueue, buf, 0) != pdTRUE) {
      // Queue full - this is acceptable, we'll catch it next time
      // Log dropped entries would only happen during extreme alert bursts
      if (DEBUG_LOGS) {
        Serial.println("[AutoLockout] Log queue full, entry dropped");
      }
    }
    return;
  }
  
  // Fall back to synchronous write
  appendLogRecordSync(buf);
}

void AutoLockoutManager::appendLogHit(const AlertEvent& event) {
  JsonDocument doc;
  doc["t"] = "hit";
  doc["ts"] = static_cast<long>(event.timestamp);
  doc["lat"] = event.latitude;
  doc["lon"] = event.longitude;
  doc["band"] = static_cast<int>(event.band);
  doc["freq"] = event.frequency_khz;
  doc["sig"] = event.signalStrength;
  doc["dur"] = event.duration_ms;
  doc["mv"] = event.isMoving;
  doc["hd"] = event.heading;
  doc["persist"] = event.isPersistent;
  appendLogRecord(doc);
}

void AutoLockoutManager::appendLogMiss(float lat, float lon, float heading, time_t ts) {
  JsonDocument doc;
  doc["t"] = "miss";
  doc["ts"] = static_cast<long>(ts);
  doc["lat"] = lat;
  doc["lon"] = lon;
  doc["hd"] = heading;
  appendLogRecord(doc);
}

void AutoLockoutManager::noteLogWrite() {
  logSinceSnapshot++;
}

void AutoLockoutManager::truncateLog() {
  fs::FS* fs = logFs();
  if (!fs) return;
  const char* path = logPath();
  File f = fs->open(path, "w");
  if (f) f.close();
  logSinceSnapshot = 0;
}

bool AutoLockoutManager::replayLog() {
  fs::FS* fs = logFs();
  if (!fs) return false;
  const char* path = logPath();
  if (!fs->exists(path)) {
    ensureLogExists();
    return false;
  }
  File f = fs->open(path, "r");
  if (!f) return false;
  replayingLog = true;
  size_t applied = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) continue;
    const char* t = doc["t"];
    if (!t) continue;
    if (strcmp(t, "hit") == 0) {
      applyHitRecord(doc);
      applied++;
    } else if (strcmp(t, "miss") == 0) {
      applyMissRecord(doc);
      applied++;
    }
  }
  f.close();
  replayingLog = false;
  if (applied > 0) {
    update();  // run promotion/demotion after rebuild
  }
  SerialLog.printf("[AutoLockout] Log replay %s (%d events)\n",
                   applied > 0 ? "applied" : "empty", (int)applied);
  return applied > 0;
}

void AutoLockoutManager::applyHitRecord(const JsonDocument& doc) {
  AlertEvent event{};
  event.latitude = doc["lat"] | 0.0f;
  event.longitude = doc["lon"] | 0.0f;
  event.heading = doc["hd"] | -1.0f;
  event.band = static_cast<Band>(doc["band"].as<int>());
  event.frequency_khz = doc["freq"] | 0;
  event.signalStrength = doc["sig"] | 0;
  event.duration_ms = doc["dur"] | 0;
  event.timestamp = doc["ts"] | time(nullptr);
  event.isMoving = doc["mv"] | false;
  event.isPersistent = doc["persist"] | false;

  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return;

  int clusterIdx = findCluster(event.latitude, event.longitude, event.band, event.frequency_khz);
  if (clusterIdx >= 0) {
    addEventToCluster(clusterIdx, event);
  } else {
    createNewCluster(event);
  }
}

void AutoLockoutManager::applyMissRecord(const JsonDocument& doc) {
  float lat = doc["lat"] | 0.0f;
  float lon = doc["lon"] | 0.0f;
  float heading = doc["hd"] | -1.0f;
  time_t ts = doc["ts"] | time(nullptr);
  recordPassthrough(lat, lon, heading, ts);
}

void AutoLockoutManager::relinkPromotedLockouts() {
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return;
  int lockoutCount = lockouts.getLockoutCount();
  for (auto& c : clusters) {
    if (!c.isPromoted) {
      c.promotedLockoutIndex = -1;
      continue;
    }
    c.promotedLockoutIndex = -1;
    for (int i = 0; i < lockoutCount; i++) {
      const Lockout* lo = lockouts.getLockoutAtIndex(i);
      if (!lo) continue;
      float dist = GPSHandler::haversineDistance(c.centerLat, c.centerLon, lo->latitude, lo->longitude);
      bool bandMatch = ( (c.band == BAND_X && lo->muteX) ||
                         (c.band == BAND_K && lo->muteK) ||
                         (c.band == BAND_KA && lo->muteKa) ||
                         (c.band == BAND_LASER && lo->muteLaser) );
      if (dist <= 250.0f && bandMatch) {
        c.promotedLockoutIndex = i;
        break;
      }
    }
  }
}

void AutoLockoutManager::maintenanceTick(unsigned long nowMs) {
  if (logSinceSnapshot >= 50 || (nowMs - lastSnapshotMs) >= (10UL * 60UL * 1000UL)) {
    // Flush any pending async log writes before snapshot
    // This ensures log file is complete before truncation
    flushLogQueue();
    saveToJSON("/v1simple/auto_lockouts.json");
    truncateLog();
    lastSnapshotMs = nowMs;
  }
}

void AutoLockoutManager::resetSessionStats() {
  sessionStats.alertsProcessed = 0;
  sessionStats.alertsSkippedWeak = 0;
  sessionStats.alertsSkippedKa = 0;
  sessionStats.alertsSkippedGps = 0;
  sessionStats.alertsSkippedDistance = 0;
  sessionStats.alertsSkippedInterval = 0;
  sessionStats.clusterHits = 0;
  sessionStats.clustersCreated = 0;
  sessionStats.clustersPromoted = 0;
  sessionStats.sessionStart = time(nullptr);
}

// ============================================================================  
// API export  
// ============================================================================

String AutoLockoutManager::exportStatusJson() const {
  JsonDocument doc;
  JsonArray arr = doc["clusters"].to<JsonArray>();

  ClusterLock lock(clusterMutex);
  if (!lock.ok()) return "{\"clusters\":[],\"session\":{}}";

  const V1Settings& s = settingsManager.get();
  int reqStopped = std::max<int>(PROMOTION_STOPPED_HIT_COUNT, s.lockoutLearnCount);
  int reqMoving  = std::max<int>(PROMOTION_MOVING_HIT_COUNT, s.lockoutLearnCount);
  int unlearnReq = s.lockoutUnlearnCount;

  for (const auto& c : clusters) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = c.name;
    o["centerLat"] = c.centerLat;
    o["centerLon"] = c.centerLon;
    o["radius_m"] = c.radius_m;
    o["band"] = static_cast<int>(c.band);
    o["frequency_khz"] = c.frequency_khz;
    o["hitCount"] = c.hitCount;
    o["stoppedHitCount"] = c.stoppedHitCount;
    o["movingHitCount"] = c.movingHitCount;
    o["firstSeen"] = static_cast<long>(c.firstSeen);
    o["lastSeen"] = static_cast<long>(c.lastSeen);
    o["passWithoutAlertCount"] = c.passWithoutAlertCount;
    o["lastPassthrough"] = static_cast<long>(c.lastPassthrough);
    o["lastCountedHit"] = static_cast<long>(c.lastCountedHit);
    o["lastCountedMiss"] = static_cast<long>(c.lastCountedMiss);
    o["isPromoted"] = c.isPromoted;
    o["promotedLockoutIndex"] = c.promotedLockoutIndex;

    int remainingStopped = std::max(0, reqStopped - c.stoppedHitCount);
    int remainingMoving  = std::max(0, reqMoving  - c.movingHitCount);
    int hitsRemaining = c.isPromoted ? 0 : std::min(remainingStopped, remainingMoving);
    int unlearnRemaining = c.isPromoted ? std::max(0, unlearnReq - c.passWithoutAlertCount) : unlearnReq;

    o["hitsRemaining"] = hitsRemaining;
    o["unlearnRemaining"] = unlearnRemaining;

    const char* status = "pending";
    if (c.isPromoted) {
      if (unlearnRemaining <= 1) status = "demoting";
      else status = "promoted";
    } else if (hitsRemaining <= 0) {
      status = "ready";
    }
    o["status"] = status;
  }

  // Add session statistics
  JsonObject session = doc["session"].to<JsonObject>();
  session["alertsProcessed"] = sessionStats.alertsProcessed;
  session["alertsSkippedWeak"] = sessionStats.alertsSkippedWeak;
  session["alertsSkippedKa"] = sessionStats.alertsSkippedKa;
  session["clusterHits"] = sessionStats.clusterHits;
  session["clustersCreated"] = sessionStats.clustersCreated;
  session["clustersPromoted"] = sessionStats.clustersPromoted;
  session["sessionStart"] = static_cast<long>(sessionStats.sessionStart);
  session["uptimeMs"] = millis();

  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================================
// Async Log Write Queue - Background SD I/O
// ============================================================================

void AutoLockoutManager::setAsyncLogMode(bool async) {
  if (async == asyncLogMode) return;  // No change
  
  if (async) {
    // Enable async mode - create queue and task if not already running
    if (!logWriteQueue) {
      logWriteQueue = xQueueCreate(LOCKOUT_LOG_QUEUE_DEPTH, LOCKOUT_LOG_LINE_SIZE);
      
      if (!logWriteQueue) {
        // Queue creation failed - stay in sync mode
        Serial.println("[AutoLockout] ERROR: Log queue alloc failed, using sync writes");
        return;
      }
      Serial.printf("[AutoLockout] Log queue created (%u items x %u bytes)\n",
                    (unsigned)LOCKOUT_LOG_QUEUE_DEPTH, (unsigned)LOCKOUT_LOG_LINE_SIZE);
    }
    
    if (!logWriterTaskHandle) {
      // Pin to Core 0 (protocol CPU) at low priority so it doesn't impact main loop on Core 1
      BaseType_t result = xTaskCreatePinnedToCore(
        logWriterTaskEntry,              // Entry function
        "LockoutLogWriter",              // Task name
        LOCKOUT_LOG_WRITER_STACK_SIZE,   // Stack size
        this,                            // Parameter (this pointer)
        1,                               // Priority (low - 1 above idle)
        &logWriterTaskHandle,            // Task handle
        0                                // Core 0 (protocol core)
      );
      
      if (result != pdPASS) {
        // Task creation failed - stay in sync mode
        Serial.println("[AutoLockout] ERROR: Failed to create log writer task");
        return;
      }
      Serial.println("[AutoLockout] Log writer task started on Core 0");
    }
    
    asyncLogMode = true;
    LOCKOUT_LOGF("[AutoLockout] Async log mode ENABLED\n");
  } else {
    // Disable async mode - flush pending items first
    flushLogQueue();
    asyncLogMode = false;
    LOCKOUT_LOGF("[AutoLockout] Async log mode DISABLED\n");
    // Note: Keep task/queue alive to avoid heap fragmentation
  }
}

void AutoLockoutManager::flushLogQueue() {
  if (!logWriteQueue) return;
  
  // Drain all pending items synchronously
  char buf[LOCKOUT_LOG_LINE_SIZE];
  while (xQueueReceive(logWriteQueue, buf, 0) == pdTRUE) {
    appendLogRecordSync(buf);
  }
}

// Static entry point for FreeRTOS task
void AutoLockoutManager::logWriterTaskEntry(void* param) {
  AutoLockoutManager* mgr = static_cast<AutoLockoutManager*>(param);
  mgr->logWriterTaskLoop();
}

// Writer task main loop - runs on Core 0, drains queue to SD
void AutoLockoutManager::logWriterTaskLoop() {
  char buf[LOCKOUT_LOG_LINE_SIZE];
  
  for (;;) {
    // Block waiting for items (portMAX_DELAY = wait forever)
    // This is fine because we're on a dedicated low-priority task
    if (xQueueReceive(logWriteQueue, buf, portMAX_DELAY) == pdTRUE) {
      // Write to SD (this is the slow part - isolated from main loop!)
      appendLogRecordSync(buf);
    }
  }
}
