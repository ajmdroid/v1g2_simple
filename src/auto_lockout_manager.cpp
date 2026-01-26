// Auto-Lockout Manager Implementation
// Intelligent false alert learning with spatial/temporal clustering

#include "auto_lockout_manager.h"
#include "gps_handler.h"
#include "lockout_manager.h"
#include "storage_manager.h"
#include "settings.h"
#include "debug_logger.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cmath>

static constexpr bool DEBUG_LOGS = false;  // Set true for verbose logging

// Lockout logging macro - logs to SD when category enabled
#define LOCKOUT_LOGF(...) do { \
    if (DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Lockout)) debugLogger.logf(DebugLogCategory::Lockout, __VA_ARGS__); \
} while(0)

// Use global instances from main.cpp
extern LockoutManager lockouts;
extern SettingsManager settingsManager;

// Atomic file write helper (same pattern as lockout_manager.cpp)
namespace {
bool writeJsonFileAtomic(fs::FS& fs, const char* path, JsonDocument& doc) {
  String tmpPath = String(path) + ".tmp";
  File tmp = fs.open(tmpPath.c_str(), "w");
  if (!tmp) {
    return false;
  }
  size_t written = serializeJson(doc, tmp);
  tmp.flush();
  tmp.close();
  if (written == 0) {
    fs.remove(tmpPath.c_str());
    return false;
  }
  fs.remove(path);
  if (!fs.rename(tmpPath.c_str(), path)) {
    // If rename fails, try to clean up
    fs.remove(tmpPath.c_str());
    return false;
  }
  return true;
}
}

AutoLockoutManager::AutoLockoutManager() : clusterMutex(nullptr), lockoutManager(nullptr) {
  clusterMutex = xSemaphoreCreateMutex();
  if (!clusterMutex) {
    Serial.println("[AutoLockout] Failed to create mutex!");
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
      Serial.printf("[AutoLockout] Skipped hit to cluster '%s' (interval: %ld sec, need: %ld sec)\n",
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
    Serial.printf("[AutoLockout] Created cluster '%s' at (%.6f, %.6f) freq: %u kHz\n",
                  cluster.name.c_str(), cluster.centerLat, cluster.centerLon, cluster.frequency_khz);
  }
}

bool AutoLockoutManager::shouldPromoteCluster(const LearningCluster& cluster) const {
  // Already promoted?
  if (cluster.isPromoted) return false;
  
  // Get runtime settings for learn count
  const V1Settings& s = settingsManager.get();
  int requiredHits = s.lockoutLearnCount;
  
  // Check hit count threshold (different for stopped vs moving)
  // Use runtime setting for the threshold
  bool hasEnoughStoppedHits = cluster.stoppedHitCount >= requiredHits;
  bool hasEnoughMovingHits = cluster.movingHitCount >= requiredHits;
  
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
  
  // Check master enable
  if (!s.lockoutEnabled) {
    return;
  }
  
  // Ka band protection (user-configurable)
  if (s.lockoutKaProtection && band == BAND_KA) {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] Not learning Ka band (protection enabled)\n");
    }
    return;
  }
  
  // Filter weak signals (likely far away or irrelevant)
  if (signalStrength < MIN_SIGNAL_STRENGTH) {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] Ignoring weak signal (strength: %d < %d)\n", 
                    signalStrength, MIN_SIGNAL_STRENGTH);
    }
    return;
  }
  
  // Filter strong signals (user-configurable, 0 = disabled)
  uint8_t maxSig = s.lockoutMaxSignalStrength;
  if (maxSig > 0 && signalStrength >= maxSig) {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] Ignoring strong signal (strength: %d >= %d)\n", 
                    signalStrength, maxSig);
    }
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
  } else {
    // Create new cluster
    createNewCluster(event);
  }
}

// Helper: Calculate angular difference between two headings (0-180 degrees)
static float headingDifference(float h1, float h2) {
  if (h1 < 0 || h2 < 0) return 0.0f;  // Unknown heading = no check
  float diff = fabs(h1 - h2);
  if (diff > 180.0f) diff = 360.0f - diff;
  return diff;
}

void AutoLockoutManager::recordPassthrough(float lat, float lon, float heading) {
  time_t now = time(nullptr);
  
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
      } else {
        if (DEBUG_LOGS) {
          Serial.printf("[AutoLockout] Skipped miss for '%s' (interval: %ld sec, need: %ld sec)\n",
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

bool AutoLockoutManager::loadFromJSON(const char* jsonPath) {
  if (!LittleFS.exists(jsonPath)) {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] No learning data at %s\n", jsonPath);
    }
    // Try to restore from SD backup
    if (checkAndRestoreFromSD()) {
      return true;
    }
    return false;
  }
  
  File file = LittleFS.open(jsonPath, "r");
  if (!file) return false;
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    if (DEBUG_LOGS) {
      Serial.printf("[AutoLockout] JSON parse error: %s\n", error.c_str());
    }
    return false;
  }
  
  // Lock for vector modifications
  ClusterLock lock(clusterMutex);
  if (!lock.ok()) {
    Serial.println("[AutoLockout] Failed to acquire mutex for load");
    return false;
  }
    clusters.clear();
  
  JsonArray clusterArray = doc["clusters"].as<JsonArray>();
  for (JsonObject obj : clusterArray) {
    LearningCluster cluster;
    cluster.name = obj["name"].as<String>();
    cluster.centerLat = obj["centerLat"].as<float>();
    cluster.centerLon = obj["centerLon"].as<float>();
    cluster.radius_m = obj["radius_m"].as<float>();
    cluster.band = static_cast<Band>(obj["band"].as<int>());
    cluster.hitCount = obj["hitCount"].as<int>();
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
    
    clusters.push_back(cluster);
  }
  
  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Loaded %d learning clusters\n", clusters.size());
  }
  
  return true;
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
      obj["hitCount"] = cluster.hitCount;
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
  
  // Atomic write: write to temp file then rename (prevents corruption on power loss)
  bool ok = writeJsonFileAtomic(LittleFS, jsonPath, doc);

  if (DEBUG_LOGS) {
    Serial.printf("[AutoLockout] Saved clusters (%d bytes)%s\n", 
                  ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  }
  
  if (!ok) {
    return false;
  }
  
  // Auto-backup to SD card if available
  backupToSD();
  
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
  saveToJSON("/v1profiles/auto_lockouts.json");
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
  saveToJSON("/v1profiles/auto_lockouts.json");
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
  bool ok = writeJsonFileAtomic(*fs, "/v1simple_auto_lockouts.json", doc);
  
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
  saveToJSON("/v1profiles/auto_lockouts.json");
  
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

