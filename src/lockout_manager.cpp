// Lockout Manager Implementation
// Manages geofence lockout zones for location-based alert muting

#include "lockout_manager.h"
#include "gps_handler.h"
#include "storage_manager.h"
#include "debug_logger.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <FS.h>
#include <memory>
#include <math.h>

// Lockout logging macro - logs to Serial AND debugLogger when Lockout category enabled
static constexpr bool LOCKOUT_DEBUG_LOGS = false;  // Set true for verbose Serial logging
#define LOCKOUT_LOG(...) do { \
    if (LOCKOUT_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Lockout)) debugLogger.logf(DebugLogCategory::Lockout, __VA_ARGS__); \
} while(0)

// Ensure the profiles directory exists on the active filesystem
static bool ensureProfilesDir(fs::FS* fs) {
  if (!fs) {
    LOCKOUT_LOG("[Lockout] ensureProfilesDir: no filesystem\n");
    return false;
  }
  if (fs->exists("/v1profiles")) return true;
  bool created = fs->mkdir("/v1profiles");
  if (!created) {
    LOCKOUT_LOG("[Lockout] ensureProfilesDir: mkdir /v1profiles FAILED\n");
  }
  return created;
}

static constexpr float MIN_RADIUS_M = 5.0f;
static constexpr float MAX_RADIUS_M = 5000.0f;  // generous upper bound
static constexpr float DUP_EPSILON = 1e-4f;     // ~11m at equator
static constexpr size_t JSON_CAPACITY_BYTES = 16384;  // Sized for dozens of lockouts

// Memory limit: ~60 bytes per lockout = ~30KB at 500 lockouts (safe for 320KB heap)
static constexpr size_t MAX_LOCKOUTS = 500;

LockoutManager::LockoutManager() : lockoutMutex(nullptr) {
  lockoutMutex = xSemaphoreCreateMutex();
  if (!lockoutMutex) {
    LOCKOUT_LOG("[Lockout] Failed to create mutex!\n");
  }
}

LockoutManager::~LockoutManager() {
  if (lockoutMutex) {
    vSemaphoreDelete(lockoutMutex);
    lockoutMutex = nullptr;
  }
}

float LockoutManager::distanceTo(float lat, float lon, const Lockout& lockout) const {
  return GPSHandler::haversineDistance(lat, lon, lockout.latitude, lockout.longitude);
}

bool LockoutManager::loadFromJSON(const char* jsonPath) {
  // =========================================================================
  // CT's "Unlocked Helpers" pattern:
  // 1. Check file existence WITHOUT lock (just a hint)
  // 2. If missing, call restore (which has its own lock)
  // 3. If found, read file with lock scoped ONLY around open/read/close
  // 4. Parse JSON WITHOUT lock (CPU work)
  // 5. Update data structures with data lock
  // Key: No function holding SDLock calls another function that takes SDLock
  // =========================================================================
  
  fs::FS* fs = storageManager.getFilesystem();
  fs::FS* lfs = storageManager.getLittleFS();
  
  // Step 1: Quick existence check (no lock needed - just a hint)
  bool primaryExists = fs && fs->exists(jsonPath);
  bool secondaryExists = lfs && lfs != fs && lfs->exists(jsonPath);
  
  if (!primaryExists && !secondaryExists) {
    LOCKOUT_LOG("[Lockout] No lockout file found at %s\n", jsonPath);
    // Step 2: Try restore from SD backup (acquires its own locks internally)
    return checkAndRestoreFromSD();
  }
  
  // Choose filesystem to use
  fs::FS* useFs = primaryExists ? fs : lfs;
  
  // Step 3: Read file with lock scoped ONLY around file I/O
  std::unique_ptr<JsonDocument> doc(new JsonDocument());
  {
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
      LOCKOUT_LOG("[Lockout] Failed to acquire SD mutex for load\n");
      return false;
    }
    
    File file = useFs->open(jsonPath, "r");
    if (!file) {
      LOCKOUT_LOG("[Lockout] Failed to open %s\n", jsonPath);
      return false;
    }

    // Bound file size to prevent heap spikes
    constexpr size_t MAX_LOCKOUT_FILE_SIZE = 150 * 1024;
    size_t fileSize = file.size();
    if (fileSize > MAX_LOCKOUT_FILE_SIZE) {
      LOCKOUT_LOG("[Lockout] WARNING: File too large (%u bytes > %u max), skipping\n",
                  (unsigned)fileSize, (unsigned)MAX_LOCKOUT_FILE_SIZE);
      file.close();
      return false;
    }

    DeserializationError error = deserializeJson(*doc, file);
    file.close();
    // SD lock released here at scope exit
    
    if (error) {
      LOCKOUT_LOG("[Lockout] JSON parse error: %s\n", error.c_str());
      return false;
    }
  }  // SD lock released
  
  // Step 4: Parse already done above, Step 5: Lock for vector modifications
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    LOCKOUT_LOG("[Lockout] Failed to acquire mutex for load\n");
    return false;
  }
  
  // Clear existing lockouts
  lockouts.clear();
  
  // Parse lockout array
  JsonArray lockoutArray = (*doc)["lockouts"].as<JsonArray>();
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
      LOCKOUT_LOG("[Lockout] Skipping invalid lockout '%s'\n", lockout.name.c_str());
      continue;
    }
    if (isDuplicate(lockout)) {
      LOCKOUT_LOG("[Lockout] Skipping duplicate lockout '%s'\n", lockout.name.c_str());
      continue;
    }
    lockouts.push_back(lockout);
  }
  
  LOCKOUT_LOG("[Lockout] Loaded %d lockout zones\n", lockouts.size());
  
  return true;
}

bool LockoutManager::saveToJSON(const char* jsonPath, bool skipBackup) {
  JsonDocument doc;
  JsonArray lockoutArray = doc["lockouts"].to<JsonArray>();
  
  // Lock for vector read access
  {
    LockoutLock lock(lockoutMutex);
    if (!lock.ok()) {
      LOCKOUT_LOG("[Lockout] Failed to acquire mutex for save\n");
      return false;
    }
    
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
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) {
    LOCKOUT_LOG("[Lockout] No filesystem available for save\n");
    return false;
  }
  
  // Acquire SD mutex to protect file I/O
  StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
  if (!sdLock) {
    LOCKOUT_LOG("[Lockout] Failed to acquire SD mutex for save\n");
    return false;
  }
  
  // Ensure directory exists - if it fails, don't try to write
  if (!ensureProfilesDir(fs)) {
    LOCKOUT_LOG("[Lockout] Cannot save: /v1profiles directory unavailable (SD=%s)\n",
                storageManager.isSDCard() ? "yes" : "no");
    return false;
  }
  
  bool ok = StorageManager::writeJsonFileAtomic(*fs, jsonPath, doc);

  LOCKOUT_LOG("[Lockout] Saved lockout zones (%d bytes)%s\n", ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  
  if (!ok) {
    return false;
  }
  
  // Secondary backup to LittleFS when SD is primary
  fs::FS* lfs = storageManager.getLittleFS();
  if (!skipBackup && lfs && lfs != fs) {
    ensureProfilesDir(lfs);
    if (!StorageManager::writeJsonFileAtomic(*lfs, jsonPath, doc)) {
      LOCKOUT_LOG("[Lockout] WARNING: LittleFS mirror write failed (SD primary OK)\n");
    }
  }
  
  // Auto-backup to SD card if primary is LittleFS
  if (!skipBackup && !storageManager.isSDCard()) {
    backupToSD();
  }
  
  return true;
}

void LockoutManager::addLockout(const Lockout& lockout) {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    LOCKOUT_LOG("[Lockout] Failed to acquire mutex for add\n");
    return;
  }
  
  // Enforce memory limit
  if (lockouts.size() >= MAX_LOCKOUTS) {
    LOCKOUT_LOG("[Lockout] Max lockout limit reached (%zu) - rejecting '%s'\n", 
                MAX_LOCKOUTS, lockout.name.c_str());
    return;
  }
  
  if (!isValidLockout(lockout)) {
    LOCKOUT_LOG("[Lockout] Rejecting invalid lockout '%s'\n", lockout.name.c_str());
    return;
  }
  if (isDuplicate(lockout)) {
    LOCKOUT_LOG("[Lockout] Rejecting duplicate lockout '%s'\n", lockout.name.c_str());
    return;
  }
  lockouts.push_back(lockout);
  
  LOCKOUT_LOG("[Lockout] Added: %s (%.6f, %.6f, %.0fm)\n",
              lockout.name.c_str(), lockout.latitude, lockout.longitude, lockout.radius_m);
}

// WARNING: If removing a lockout that was auto-promoted from AutoLockoutManager,
// use AutoLockoutManager::demoteCluster() instead, which properly updates
// promotedLockoutIndex for other clusters. Direct removal here will cause
// stale indices in AutoLockoutManager until relinkPromotedLockouts() is called.
// If a removal callback is registered, it will be called after removal to allow
// index synchronization.
void LockoutManager::removeLockout(int index) {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    LOCKOUT_LOG("[Lockout] Failed to acquire mutex for remove\n");
    return;
  }
  
  if (index >= 0 && index < (int)lockouts.size()) {
    LOCKOUT_LOG("[Lockout] Removed: %s\n", lockouts[index].name.c_str());
    lockouts.erase(lockouts.begin() + index);
    
    // Notify AutoLockoutManager to update indices (if callback registered)
    // WARNING: Callback is invoked with lockoutMutex held.
    // Do NOT call any LockoutManager methods from the callback or deadlock will occur.
    if (onLockoutRemovedCallback) {
      onLockoutRemovedCallback(index);
    }
  }
}

void LockoutManager::updateLockout(int index, const Lockout& lockout) {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    LOCKOUT_LOG("[Lockout] Failed to acquire mutex for update\n");
    return;
  }
  
  if (index >= 0 && index < (int)lockouts.size()) {
    if (!isValidLockout(lockout)) {
      LOCKOUT_LOG("[Lockout] Rejecting invalid update for '%s'\n", lockout.name.c_str());
      return;
    }
    if (isDuplicate(lockout, index)) {
      LOCKOUT_LOG("[Lockout] Rejecting duplicate update for '%s'\n", lockout.name.c_str());
      return;
    }
    lockouts[index] = lockout;
    
    LOCKOUT_LOG("[Lockout] Updated: %s\n", lockout.name.c_str());
  }
}

void LockoutManager::clearAll() {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    LOCKOUT_LOG("[Lockout] Failed to acquire mutex for clearAll\n");
    return;
  }
  
  LOCKOUT_LOG("[Lockout] Cleared all %d lockouts\n", lockouts.size());
  lockouts.clear();
}

int LockoutManager::getLockoutCount() const {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    return 0;
  }
  return static_cast<int>(lockouts.size());
}

const Lockout* LockoutManager::getLockoutAtIndex(int idx) const {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    return nullptr;
  }
  
  if (idx >= 0 && idx < (int)lockouts.size()) {
    return &lockouts[idx];
  }
  return nullptr;
}

int LockoutManager::getNearestLockout(float lat, float lon) const {
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    return -1;
  }
  
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
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    return false;  // Fail open - don't mute if we can't check
  }
  
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
      LOCKOUT_LOG("[Lockout] Muting alert (inside '%s', %.0fm from center)\n",
                  lockout.name.c_str(), dist);
      return true;
    }
  }
  
  return false;
}

std::vector<int> LockoutManager::getActiveLockouts(float lat, float lon) const {
  std::vector<int> activeLockouts;
  
  LockoutLock lock(lockoutMutex);
  if (!lock.ok()) {
    return activeLockouts;  // Return empty vector
  }
  
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
    LOCKOUT_LOG("[Lockout] SD card not available for backup\n");
    return false;
  }
  
  // Acquire SD mutex to protect file I/O
  StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
  if (!sdLock) {
    LOCKOUT_LOG("[Lockout] Failed to acquire SD mutex for backup\n");
    return false;
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) return false;
  
  JsonDocument doc;
  doc["_type"] = "v1simple_lockouts_backup";
  doc["_version"] = 1;
  doc["timestamp"] = millis();
  
  JsonArray lockoutArray = doc["lockouts"].to<JsonArray>();
  
  // Lock for vector read access
  {
    LockoutLock lock(lockoutMutex);
    if (!lock.ok()) {
      LOCKOUT_LOG("[Lockout] Failed to acquire mutex for SD backup\n");
      return false;
    }
    
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
  }
  
  bool ok = StorageManager::writeJsonFileAtomic(*fs, "/v1simple_lockouts.json", doc);
  
  LOCKOUT_LOG("[Lockout] Backed up lockouts to SD (%d bytes)%s\n", 
              ok ? measureJson(doc) : 0, ok ? "" : " [FAILED]");
  
  return ok;
}

bool LockoutManager::restoreFromSD() {
  if (!storageManager.isReady() || !storageManager.isSDCard()) {
    return false;
  }
  
  fs::FS* fs = storageManager.getFilesystem();
  if (!fs) return false;
  
  // =========================================================================
  // CT's "Unlocked Helpers" pattern:
  // Lock ONLY around file I/O, release before calling saveToJSON
  // =========================================================================
  
  JsonDocument doc;
  
  // Step 1: Read file with lock scoped ONLY around file I/O
  {
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
    if (!sdLock) {
      LOCKOUT_LOG("[Lockout] Failed to acquire SD mutex for restore\n");
      return false;
    }
    
    if (!fs->exists("/v1simple_lockouts.json")) {
      return false;
    }
    
    File file = fs->open("/v1simple_lockouts.json", "r");
    if (!file) {
      return false;
    }
    
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    // SD lock released here at scope exit
    
    if (error) {
      LOCKOUT_LOG("[Lockout] SD backup parse error: %s\n", error.c_str());
      return false;
    }
  }  // SD lock released before any other lock-taking calls
  
  // Step 2: Validate format (no lock needed - CPU work)
  if (doc["_type"] != "v1simple_lockouts_backup") {
    LOCKOUT_LOG("[Lockout] Invalid SD backup format\n");
    return false;
  }
  
  // Step 3: Update data structures with data lock (not SD lock)
  {
    LockoutLock lock(lockoutMutex);
    if (!lock.ok()) {
      LOCKOUT_LOG("[Lockout] Failed to acquire mutex for SD restore\n");
      return false;
    }
    
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
        LOCKOUT_LOG("[Lockout] Skipping invalid/duplicate lockout '%s' from SD backup\n", lockout.name.c_str());
        continue;
      }

      lockouts.push_back(lockout);
    }
    
    LOCKOUT_LOG("[Lockout] Restored %d lockouts from SD backup\n", lockouts.size());
  }  // Data lock released
  
  // Step 4: Save to LittleFS (acquires its own SD lock internally)
  saveToJSON("/v1profiles/lockouts.json", true);  // Skip re-backup while restoring
  
  return true;
}

bool LockoutManager::checkAndRestoreFromSD() {
  // Check if LittleFS is empty and SD backup exists
  if (lockouts.empty() && storageManager.isSDCard()) {
    LOCKOUT_LOG("[Lockout] LittleFS empty, checking for SD backup...\n");
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
