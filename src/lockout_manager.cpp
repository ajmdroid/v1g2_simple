// Lockout Manager Implementation
// Manages geofence lockout zones for location-based alert muting

#include "lockout_manager.h"
#include "gps_handler.h"
#include "storage_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>
#include <math.h>

static constexpr bool DEBUG_LOGS = false;  // Set true for verbose logging
static constexpr float MIN_RADIUS_M = 5.0f;
static constexpr float MAX_RADIUS_M = 5000.0f;  // generous upper bound
static constexpr float DUP_EPSILON = 1e-4f;     // ~11m at equator
static constexpr size_t JSON_CAPACITY_BYTES = 16384;  // Sized for dozens of lockouts

// Memory limit: ~60 bytes per lockout = ~30KB at 500 lockouts (safe for 320KB heap)
static constexpr size_t MAX_LOCKOUTS = 500;

namespace {
bool writeJsonFileAtomic(fs::FS& fs, const char* path, DynamicJsonDocument& doc) {
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

LockoutManager::LockoutManager() {
  // Constructor
}

LockoutManager::~LockoutManager() {
  // Destructor
}

float LockoutManager::distanceTo(float lat, float lon, const Lockout& lockout) const {
  return GPSHandler::haversineDistance(lat, lon, lockout.latitude, lockout.longitude);
}

bool LockoutManager::loadFromJSON(const char* jsonPath) {
  if (!LittleFS.exists(jsonPath)) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] No lockout file found at %s\n", jsonPath);
    }
    // Try to restore from SD backup
    if (checkAndRestoreFromSD()) {
      return true;
    }
    return false;
  }
  
  File file = LittleFS.open(jsonPath, "r");
  if (!file) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] Failed to open %s\n", jsonPath);
    }
    return false;
  }

  // Use bounded JSON document to avoid heap churn; sized for typical lockout counts
  DynamicJsonDocument doc(JSON_CAPACITY_BYTES);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] JSON parse error: %s\n", error.c_str());
    }
    return false;
  }
  
  // Clear existing lockouts
  lockouts.clear();
  
  // Parse lockout array
  JsonArray lockoutArray = doc["lockouts"].as<JsonArray>();
  for (JsonObject obj : lockoutArray) {
    Lockout lockout;
    lockout.name = obj["name"].as<String>();
    lockout.latitude = obj["latitude"].as<float>();
    lockout.longitude = obj["longitude"].as<float>();
    lockout.radius_m = obj["radius_m"].as<float>();
    lockout.enabled = obj["enabled"].as<bool>();
    lockout.muteX = obj["muteX"] | false;  // Default false if missing
    lockout.muteK = obj["muteK"] | false;
    lockout.muteKa = obj["muteKa"] | false;
    lockout.muteLaser = obj["muteLaser"] | false;

    if (!isValidLockout(lockout)) {
      if (DEBUG_LOGS) {
        Serial.printf("[Lockout] Skipping invalid lockout '%s'\n", lockout.name.c_str());
      }
      continue;
    }
    if (isDuplicate(lockout)) {
      if (DEBUG_LOGS) {
        Serial.printf("[Lockout] Skipping duplicate lockout '%s'\n", lockout.name.c_str());
      }
      continue;
    }
    lockouts.push_back(lockout);
  }
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Loaded %d lockout zones\n", lockouts.size());
  }
  
  return true;
}

bool LockoutManager::saveToJSON(const char* jsonPath, bool skipBackup) {
  DynamicJsonDocument doc(JSON_CAPACITY_BYTES);
  JsonArray lockoutArray = doc["lockouts"].to<JsonArray>();
  
  for (const auto& lockout : lockouts) {
    JsonObject obj = lockoutArray.add<JsonObject>();
    obj["name"] = lockout.name;
    obj["latitude"] = lockout.latitude;
    obj["longitude"] = lockout.longitude;
    obj["radius_m"] = lockout.radius_m;
    obj["enabled"] = lockout.enabled;
    obj["muteX"] = lockout.muteX;
    obj["muteK"] = lockout.muteK;
    obj["muteKa"] = lockout.muteKa;
    obj["muteLaser"] = lockout.muteLaser;
  }
  
  bool ok = writeJsonFileAtomic(LittleFS, jsonPath, doc);

  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Saved %d lockout zones (%d bytes)%s\n", lockouts.size(), ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  }
  
  if (!ok) {
    return false;
  }
  
  // Auto-backup to SD card if available (unless explicitly skipped)
  if (!skipBackup) {
    backupToSD();
  }
  
  return true;
}

void LockoutManager::addLockout(const Lockout& lockout) {
  // Enforce memory limit
  if (lockouts.size() >= MAX_LOCKOUTS) {
    Serial.printf("[Lockout] Max lockout limit reached (%zu) - rejecting '%s'\n", 
                  MAX_LOCKOUTS, lockout.name.c_str());
    return;
  }
  
  if (!isValidLockout(lockout)) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] Rejecting invalid lockout '%s'\n", lockout.name.c_str());
    }
    return;
  }
  if (isDuplicate(lockout)) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] Rejecting duplicate lockout '%s'\n", lockout.name.c_str());
    }
    return;
  }
  lockouts.push_back(lockout);
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Added: %s (%.6f, %.6f, %.0fm)\n",
                  lockout.name.c_str(), lockout.latitude, lockout.longitude, lockout.radius_m);
  }
}

void LockoutManager::removeLockout(int index) {
  if (index >= 0 && index < (int)lockouts.size()) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] Removed: %s\n", lockouts[index].name.c_str());
    }
    lockouts.erase(lockouts.begin() + index);
  }
}

void LockoutManager::updateLockout(int index, const Lockout& lockout) {
  if (index >= 0 && index < (int)lockouts.size()) {
    if (!isValidLockout(lockout)) {
      if (DEBUG_LOGS) {
        Serial.printf("[Lockout] Rejecting invalid update for '%s'\n", lockout.name.c_str());
      }
      return;
    }
    if (isDuplicate(lockout, index)) {
      if (DEBUG_LOGS) {
        Serial.printf("[Lockout] Rejecting duplicate update for '%s'\n", lockout.name.c_str());
      }
      return;
    }
    lockouts[index] = lockout;
    
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] Updated: %s\n", lockout.name.c_str());
    }
  }
}

void LockoutManager::clearAll() {
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Cleared all %d lockouts\n", lockouts.size());
  }
  lockouts.clear();
}

const Lockout* LockoutManager::getLockoutAtIndex(int idx) const {
  if (idx >= 0 && idx < (int)lockouts.size()) {
    return &lockouts[idx];
  }
  return nullptr;
}

int LockoutManager::getNearestLockout(float lat, float lon) const {
  int nearestIdx = -1;
  float minDistance = 999999.0f;
  
  for (size_t i = 0; i < lockouts.size(); i++) {
    float dist = distanceTo(lat, lon, lockouts[i]);
    if (dist < minDistance) {
      minDistance = dist;
      nearestIdx = i;
    }
  }
  
  return nearestIdx;
}

bool LockoutManager::shouldMuteAlert(float lat, float lon, Band band) const {
  for (const auto& lockout : lockouts) {
    if (!lockout.enabled) continue;
    
    // Calculate distance to lockout center
    float dist = distanceTo(lat, lon, lockout);
    
    // Check if we're inside the lockout radius
    if (dist > lockout.radius_m) continue;
    
    // Check if this lockout mutes the given band
    bool shouldMute = false;
    switch (band) {
      case BAND_X:
        shouldMute = lockout.muteX;
        break;
      case BAND_K:
        shouldMute = lockout.muteK;
        break;
      case BAND_KA:
        shouldMute = lockout.muteKa;
        break;
      case BAND_LASER:
        shouldMute = lockout.muteLaser;
        break;
      default:
        shouldMute = false;
    }
    
    if (shouldMute) {
      if (DEBUG_LOGS) {
        Serial.printf("[Lockout] Muting alert (inside '%s', %.0fm from center)\n",
                      lockout.name.c_str(), dist);
      }
      return true;
    }
  }
  
  return false;
}

std::vector<int> LockoutManager::getActiveLockouts(float lat, float lon) const {
  std::vector<int> activeLockouts;
  
  for (size_t i = 0; i < lockouts.size(); i++) {
    if (!lockouts[i].enabled) continue;
    
    float dist = distanceTo(lat, lon, lockouts[i]);
    if (dist <= lockouts[i].radius_m) {
      activeLockouts.push_back(i);
    }
  }
  
  return activeLockouts;
}

// ============================================================================
// SD Card Backup/Restore (survives firmware updates)
// ============================================================================

bool LockoutManager::backupToSD() {
  if (!storageManager.isReady() || !storageManager.isSDCard()) {
    if (DEBUG_LOGS) {
      Serial.println("[Lockout] SD card not available for backup");
    }
    return false;
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) return false;
  
  DynamicJsonDocument doc(JSON_CAPACITY_BYTES);
  doc["_type"] = "v1simple_lockouts_backup";
  doc["_version"] = 1;
  doc["timestamp"] = millis();
  
  JsonArray lockoutArray = doc["lockouts"].to<JsonArray>();
  
  for (const auto& lockout : lockouts) {
    JsonObject obj = lockoutArray.add<JsonObject>();
    obj["name"] = lockout.name;
    obj["latitude"] = lockout.latitude;
    obj["longitude"] = lockout.longitude;
    obj["radius_m"] = lockout.radius_m;
    obj["enabled"] = lockout.enabled;
    obj["muteX"] = lockout.muteX;
    obj["muteK"] = lockout.muteK;
    obj["muteKa"] = lockout.muteKa;
    obj["muteLaser"] = lockout.muteLaser;
  }
  
  bool ok = writeJsonFileAtomic(*fs, "/v1simple_lockouts.json", doc);
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Backed up %d lockouts to SD (%d bytes)%s\n", 
                  lockouts.size(), ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  }
  
  return ok;
}

bool LockoutManager::restoreFromSD() {
  if (!storageManager.isReady() || !storageManager.isSDCard()) {
    return false;
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) return false;
  
  if (!fs->exists("/v1simple_lockouts.json")) {
    return false;
  }
  
  File file = fs->open("/v1simple_lockouts.json", "r");
  if (!file) {
    return false;
  }
  
  DynamicJsonDocument doc(JSON_CAPACITY_BYTES);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] SD backup parse error: %s\n", error.c_str());
    }
    return false;
  }
  
  // Verify backup format
  if (doc["_type"] != "v1simple_lockouts_backup") {
    if (DEBUG_LOGS) {
      Serial.println("[Lockout] Invalid SD backup format");
    }
    return false;
  }
  
  // Clear and restore
  lockouts.clear();
  
  JsonArray lockoutArray = doc["lockouts"].as<JsonArray>();
  for (JsonObject obj : lockoutArray) {
    Lockout lockout;
    lockout.name = obj["name"].as<String>();
    lockout.latitude = obj["latitude"].as<float>();
    lockout.longitude = obj["longitude"].as<float>();
    lockout.radius_m = obj["radius_m"].as<float>();
    lockout.enabled = obj["enabled"].as<bool>();
    lockout.muteX = obj["muteX"] | false;
    lockout.muteK = obj["muteK"] | false;
    lockout.muteKa = obj["muteKa"] | false;
    lockout.muteLaser = obj["muteLaser"] | false;

    if (!isValidLockout(lockout) || isDuplicate(lockout)) {
      if (DEBUG_LOGS) {
        Serial.printf("[Lockout] Skipping invalid/duplicate lockout '%s' from SD backup\n", lockout.name.c_str());
      }
      continue;
    }

    lockouts.push_back(lockout);
  }
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Restored %d lockouts from SD backup\n", lockouts.size());
  }
  
  // Save to LittleFS
  saveToJSON("/v1profiles/lockouts.json", true);  // Skip re-backup while restoring
  
  return true;
}

bool LockoutManager::checkAndRestoreFromSD() {
  // Check if LittleFS is empty and SD backup exists
  if (lockouts.empty() && storageManager.isSDCard()) {
    if (DEBUG_LOGS) {
      Serial.println("[Lockout] LittleFS empty, checking for SD backup...");
    }
    return restoreFromSD();
  }
  return false;
}

bool LockoutManager::isValidLockout(const Lockout& lockout) const {
  if (!isfinite(lockout.latitude) || !isfinite(lockout.longitude) || !isfinite(lockout.radius_m)) {
    return false;
  }
  if (lockout.latitude < -90.0f || lockout.latitude > 90.0f) return false;
  if (lockout.longitude < -180.0f || lockout.longitude > 180.0f) return false;
  if (lockout.radius_m < MIN_RADIUS_M || lockout.radius_m > MAX_RADIUS_M) return false;
  return true;
}

bool LockoutManager::isDuplicate(const Lockout& lockout, int ignoreIndex) const {
  for (size_t i = 0; i < lockouts.size(); i++) {
    if ((int)i == ignoreIndex) continue;
    const auto& existing = lockouts[i];
    if (fabs(existing.latitude - lockout.latitude) < DUP_EPSILON &&
        fabs(existing.longitude - lockout.longitude) < DUP_EPSILON &&
        fabs(existing.radius_m - lockout.radius_m) < 1.0f) {
      // Consider matching if same location/radius and same band flags
      if (existing.muteX == lockout.muteX && existing.muteK == lockout.muteK &&
          existing.muteKa == lockout.muteKa && existing.muteLaser == lockout.muteLaser) {
        return true;
      }
    }
  }
  return false;
}
