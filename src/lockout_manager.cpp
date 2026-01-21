// Lockout Manager Implementation
// Manages geofence lockout zones for location-based alert muting

#include "lockout_manager.h"
#include "gps_handler.h"
#include "storage_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static constexpr bool DEBUG_LOGS = false;  // Set true for verbose logging

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
  
  JsonDocument doc;
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
    
    lockouts.push_back(lockout);
  }
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Loaded %d lockout zones\n", lockouts.size());
  }
  
  return true;
}

bool LockoutManager::saveToJSON(const char* jsonPath) {
  JsonDocument doc;
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
  
  File file = LittleFS.open(jsonPath, "w");
  if (!file) {
    if (DEBUG_LOGS) {
      Serial.printf("[Lockout] Failed to open %s for writing\n", jsonPath);
    }
    return false;
  }
  
  size_t bytesWritten = serializeJson(doc, file);
  file.close();
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Saved %d lockout zones (%d bytes)\n", lockouts.size(), bytesWritten);
  }
  
  // Auto-backup to SD card if available
  backupToSD();
  
  return bytesWritten > 0;
}

void LockoutManager::addLockout(const Lockout& lockout) {
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
  
  JsonDocument doc;
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
  
  File file = fs->open("/v1simple_lockouts.json", "w");
  if (!file) {
    if (DEBUG_LOGS) {
      Serial.println("[Lockout] Failed to open SD file for backup");
    }
    return false;
  }
  
  size_t written = serializeJson(doc, file);
  file.close();
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Backed up %d lockouts to SD (%d bytes)\n", 
                  lockouts.size(), written);
  }
  
  return written > 0;
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
  
  JsonDocument doc;
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
    
    lockouts.push_back(lockout);
  }
  
  if (DEBUG_LOGS) {
    Serial.printf("[Lockout] Restored %d lockouts from SD backup\n", lockouts.size());
  }
  
  // Save to LittleFS
  saveToJSON("/v1profiles/lockouts.json");
  
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
