// Camera Alert Manager Implementation
// Loads NDJSON camera database and provides fast proximity queries

#include "camera_manager.h"
#include "debug_logger.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

static constexpr bool DEBUG_LOGS = false;

// Camera logging macro - logs to SD when category enabled
#define CAMERA_LOGF(...) do { \
    if (DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Camera)) debugLogger.logf(DebugLogCategory::Camera, __VA_ARGS__); \
} while(0)

// Global instance
CameraManager cameraManager;

CameraManager::CameraManager() {
  // Reserve space for typical database size
  cameras.reserve(1000);
  
  // Create mutex for thread-safe access to camera vector
  cameraMutex = xSemaphoreCreateMutex();
  if (!cameraMutex) {
    Serial.println("[Camera] WARNING: Failed to create camera mutex!");
  }
}

CameraManager::~CameraManager() {
  // Stop any background loading
  stopBackgroundLoad();
  
  // Delete mutex
  if (cameraMutex) {
    vSemaphoreDelete(cameraMutex);
    cameraMutex = nullptr;
  }
  
  clear();
}

bool CameraManager::begin(fs::FS* filesystem) {
  fs = filesystem;
  if (!fs) {
    Serial.println("[Camera] No filesystem provided");
    return false;
  }
  
  // Clear any existing data first
  clear();
  
  // Try to load default database from SD card
  // Load all available camera database files
  bool loaded = false;
  
  // Primary camera database files (ALPR, red light, speed)
  if (fs->exists("/alpr.json")) {
    loaded = loadDatabase("/alpr.json", loaded) || loaded;  // First file clears, subsequent append
  }
  if (fs->exists("/redlight_cam.json")) {
    loaded = loadDatabase("/redlight_cam.json", true) || loaded;
  }
  if (fs->exists("/speed_cam.json")) {
    loaded = loadDatabase("/speed_cam.json", true) || loaded;
  }
  
  // Legacy/alternative file names (only if no primary files found)
  if (!loaded) {
    if (fs->exists("/cameras.json")) {
      return loadDatabase("/cameras.json", false);
    } else if (fs->exists("/alpr_osm.json")) {
      // OSM ALPR data uploaded via web UI
      return loadDatabase("/alpr_osm.json", false);
    } else if (fs->exists("/V140ExCam.json")) {
      return loadDatabase("/V140ExCam.json", false);
    } else if (fs->exists("/excam.json")) {
      return loadDatabase("/excam.json", false);
    }
  }
  
  if (!loaded && DEBUG_LOGS) {
    Serial.println("[Camera] No camera database found on SD card");
  }
  
  if (loaded) {
    Serial.printf("[Camera] Total cameras loaded: %d\n", cameras.size());
  }
  
  return loaded;
}

bool CameraManager::loadDatabase(const char* path, bool append) {
  if (!fs) return false;
  
  File file = fs->open(path, "r");
  if (!file) {
    Serial.printf("[Camera] Failed to open %s\n", path);
    return false;
  }
  
  // Clear existing data only if not appending
  if (!append) {
    clear();
  }
  
  Serial.printf("[Camera] Loading database from %s...\n", path);
  uint32_t startTime = millis();
  size_t startCount = cameras.size();
  
  size_t lineCount = 0;
  size_t parseErrors = 0;
  char lineBuffer[256];
  size_t bufIdx = 0;
  
  while (file.available()) {
    char c = file.read();
    
    if (c == '\n' || c == '\r') {
      if (bufIdx > 0) {
        lineBuffer[bufIdx] = '\0';
        
        CameraRecord record;
        if (parseCameraLine(lineBuffer, record)) {
          cameras.push_back(record);
        } else {
          parseErrors++;
        }
        
        lineCount++;
        bufIdx = 0;
        
        // Progress indicator every 1000 lines
        if (DEBUG_LOGS && lineCount % 1000 == 0) {
          Serial.printf("[Camera] Loaded %d cameras...\n", cameras.size());
        }
      }
    } else if (bufIdx < sizeof(lineBuffer) - 1) {
      lineBuffer[bufIdx++] = c;
    }
  }
  
  // Handle last line without newline
  if (bufIdx > 0) {
    lineBuffer[bufIdx] = '\0';
    CameraRecord record;
    if (parseCameraLine(lineBuffer, record)) {
      cameras.push_back(record);
    }
    lineCount++;
  }
  
  file.close();
  
  // Build spatial index for fast queries
  buildSpatialIndex();
  
  size_t addedCount = cameras.size() - startCount;
  uint32_t elapsed = millis() - startTime;
  Serial.printf("[Camera] Loaded %d cameras from %s in %dms (%d parse errors)\n", 
                addedCount, path, elapsed, parseErrors);
  
  // Log type breakdown
  if (DEBUG_LOGS) {
    Serial.printf("[Camera] Types: %d red light, %d speed, %d ALPR\n",
                  getRedLightCount(), getSpeedCameraCount(), getALPRCount());
  }
  
  return addedCount > 0;
}

bool CameraManager::parseCameraLine(const char* line, CameraRecord& record) {
  // Skip empty lines
  if (!line || line[0] == '\0') return false;
  
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, line);
  
  if (error) {
    // Not JSON - might be metadata line or CSV
    return false;
  }
  
  // Check for metadata line (has _meta field) - use is<T>() for ArduinoJson 7.x
  if (doc["_meta"].is<JsonObject>()) {
    JsonObject meta = doc["_meta"];
    if (meta["name"].is<const char*>()) {
      strncpy(databaseName, meta["name"] | "", sizeof(databaseName) - 1);
    }
    if (meta["date"].is<const char*>()) {
      strncpy(databaseDate, meta["date"] | "", sizeof(databaseDate) - 1);
    }
    Serial.printf("[Camera] Database: %s (%s)\n", databaseName, databaseDate);
    return false;  // Not a camera record
  }
  
  // Must have lat/lon - use is<T>() for ArduinoJson 7.x
  if (!doc["lat"].is<float>() || !doc["lon"].is<float>()) {
    return false;
  }
  
  record.latitude = doc["lat"].as<float>();
  record.longitude = doc["lon"].as<float>();
  
  // Validate coordinates
  if (record.latitude < -90.0f || record.latitude > 90.0f ||
      record.longitude < -180.0f || record.longitude > 180.0f) {
    return false;
  }
  
  // Camera type from flg field
  // Note: flg values 1-3 are standard ExCam types, but ALPR uses flg=8192
  // We map 8192 to our internal ALPR type (4) since uint8_t can't hold 8192
  int flg = doc["flg"] | 2;  // Default to speed camera
  if (flg == 8192) {
    record.type = static_cast<uint8_t>(CameraType::ALPR);
  } else {
    record.type = static_cast<uint8_t>(flg);
  }
  
  // Speed limit (0 if not specified)
  record.speedLimit = doc["spd"] | 0;
  
  // Unit (default to mph for US)
  const char* unt = doc["unt"] | "mph";
  record.isMetric = (strcmp(unt, "kmh") == 0);
  
  // Directions array - use is<T>() for ArduinoJson 7.x
  record.directionCount = 0;
  if (doc["dir"].is<JsonArray>()) {
    JsonArray dirs = doc["dir"].as<JsonArray>();
    for (JsonVariant d : dirs) {
      if (record.directionCount < 2) {
        record.directions[record.directionCount++] = d.as<uint16_t>();
      }
    }
  }
  
  return true;
}

void CameraManager::clear() {
  cameras.clear();
  regionalCache.clear();
  spatialIndex.clear();
  indexBuilt = false;
  databaseName[0] = '\0';
  databaseDate[0] = '\0';
  cacheCenterLat = 0.0f;
  cacheCenterLon = 0.0f;
  cacheRadiusMi = 0.0f;
  cacheBuiltMs = 0;
}

// Helper to get the active camera list (regional cache if available, else full DB)
const std::vector<CameraRecord>& CameraManager::getQueryCameras() const {
  if (!regionalCache.empty()) {
    return regionalCache;
  }
  return cameras;
}

// Build regional cache containing only cameras within radius of GPS position
// Thread-safe: uses mutex when accessing full camera database
bool CameraManager::buildRegionalCache(float lat, float lon, float radiusMiles) {
  // Take mutex to safely read cameras vector (may be modified by background task)
  if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[Camera] Cannot build cache - mutex unavailable");
    return false;
  }
  
  if (cameras.empty()) {
    xSemaphoreGive(cameraMutex);
    Serial.println("[Camera] Cannot build cache - no database loaded");
    return false;
  }
  
  uint32_t startTime = millis();
  regionalCache.clear();
  
  // Convert miles to meters for haversine
  float radiusM = radiusMiles * 1609.34f;
  
  // Pre-calculate bounding box for quick filtering
  float latDelta = radiusM / 111000.0f;
  float lonDelta = radiusM / (111000.0f * cos(lat * PI / 180.0f));
  
  // Copy cameras within radius
  for (const auto& cam : cameras) {
    // Quick bounding box check
    if (fabs(cam.latitude - lat) > latDelta) continue;
    if (fabs(cam.longitude - lon) > lonDelta) continue;
    
    // Precise distance check
    float dist = haversineDistance(lat, lon, cam.latitude, cam.longitude);
    if (dist <= radiusM) {
      regionalCache.push_back(cam);
    }
  }
  
  // Store cache metadata
  cacheCenterLat = lat;
  cacheCenterLon = lon;
  cacheRadiusMi = radiusMiles;
  cacheBuiltMs = millis();
  
  // Release mutex now that we're done reading cameras
  xSemaphoreGive(cameraMutex);
  
  uint32_t elapsed = millis() - startTime;
  Serial.printf("[Camera] Regional cache: %d of %d cameras within %.0f mi (took %dms)\n",
                regionalCache.size(), cameras.size(), radiusMiles, elapsed);
  
  return !regionalCache.empty();
}

// Check if cache needs refresh based on distance from cache center
bool CameraManager::needsCacheRefresh(float lat, float lon, float distanceThresholdMiles) const {
  // No cache exists - needs refresh
  if (regionalCache.empty() || cacheRadiusMi == 0.0f) {
    return true;
  }
  
  // Check distance from cache center
  float distM = haversineDistance(lat, lon, cacheCenterLat, cacheCenterLon);
  float distMiles = distM / 1609.34f;
  
  // If moved more than threshold, refresh needed
  if (distMiles > distanceThresholdMiles) {
    if (DEBUG_LOGS) {
      Serial.printf("[Camera] Cache refresh needed: moved %.1f mi from cache center\n", distMiles);
    }
    return true;
  }
  
  return false;
}

// Save regional cache to LittleFS for fast boot
bool CameraManager::saveRegionalCache(fs::FS* filesystem, const char* path) {
  if (regionalCache.empty()) {
    Serial.println("[Camera] No regional cache to save");
    return false;
  }
  
  if (!filesystem) {
    Serial.println("[Camera] No filesystem for cache save");
    return false;
  }
  
  uint32_t startTime = millis();
  
  File file = filesystem->open(path, "w");
  if (!file) {
    Serial.printf("[Camera] Failed to open %s for writing\n", path);
    return false;
  }
  
  // Write metadata line first
  char metaLine[128];
  snprintf(metaLine, sizeof(metaLine), 
           "{\"_cache\":{\"lat\":%.6f,\"lon\":%.6f,\"radius\":%.1f,\"count\":%d}}\n",
           cacheCenterLat, cacheCenterLon, cacheRadiusMi, regionalCache.size());
  file.print(metaLine);
  
  // Write camera records as NDJSON
  char line[128];
  for (const auto& cam : regionalCache) {
    int len = snprintf(line, sizeof(line), 
                       "{\"lat\":%.6f,\"lon\":%.6f,\"flg\":%d",
                       cam.latitude, cam.longitude, cam.type);
    
    if (cam.speedLimit > 0) {
      len += snprintf(line + len, sizeof(line) - len, ",\"spd\":%d", cam.speedLimit);
    }
    
    if (cam.directionCount > 0) {
      len += snprintf(line + len, sizeof(line) - len, ",\"dir\":[%d", cam.directions[0]);
      if (cam.directionCount > 1) {
        len += snprintf(line + len, sizeof(line) - len, ",%d", cam.directions[1]);
      }
      len += snprintf(line + len, sizeof(line) - len, "]");
    }
    
    len += snprintf(line + len, sizeof(line) - len, "}\n");
    file.print(line);
  }
  
  file.close();
  
  uint32_t elapsed = millis() - startTime;
  Serial.printf("[Camera] Saved %d cameras to cache in %dms\n", regionalCache.size(), elapsed);
  
  return true;
}

// Load regional cache from LittleFS (for fast boot before full DB load)
bool CameraManager::loadRegionalCache(fs::FS* filesystem, const char* path) {
  if (!filesystem) {
    return false;
  }
  
  if (!filesystem->exists(path)) {
    if (DEBUG_LOGS) {
      Serial.printf("[Camera] No cache file at %s\n", path);
    }
    return false;
  }
  
  File file = filesystem->open(path, "r");
  if (!file) {
    return false;
  }
  
  uint32_t startTime = millis();
  regionalCache.clear();
  
  char lineBuffer[256];
  size_t bufIdx = 0;
  bool metaFound = false;
  
  while (file.available()) {
    char c = file.read();
    
    if (c == '\n' || c == '\r') {
      if (bufIdx > 0) {
        lineBuffer[bufIdx] = '\0';
        
        // Parse line
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, lineBuffer);
        
        if (!error) {
          // Check for metadata line
          if (doc["_cache"].is<JsonObject>()) {
            JsonObject meta = doc["_cache"];
            cacheCenterLat = meta["lat"] | 0.0f;
            cacheCenterLon = meta["lon"] | 0.0f;
            cacheRadiusMi = meta["radius"] | 0.0f;
            metaFound = true;
          } else {
            // Regular camera record
            CameraRecord record;
            if (parseCameraLine(lineBuffer, record)) {
              regionalCache.push_back(record);
            }
          }
        }
        
        bufIdx = 0;
      }
    } else if (bufIdx < sizeof(lineBuffer) - 1) {
      lineBuffer[bufIdx++] = c;
    }
  }
  
  file.close();
  
  if (regionalCache.empty() || !metaFound) {
    Serial.println("[Camera] Cache load failed - invalid or empty cache");
    regionalCache.clear();
    return false;
  }
  
  cacheBuiltMs = millis();
  uint32_t elapsed = millis() - startTime;
  Serial.printf("[Camera] Loaded %d cached cameras in %dms (center: %.4f,%.4f)\n",
                regionalCache.size(), elapsed, cacheCenterLat, cacheCenterLon);
  
  return true;
}

void CameraManager::buildSpatialIndex() {
  // For now, skip complex spatial indexing
  // The brute-force search is fast enough for <20k cameras
  // A spatial index would be needed for >100k cameras
  indexBuilt = true;
  
  if (DEBUG_LOGS) {
    Serial.printf("[Camera] Spatial index ready for %d cameras\n", cameras.size());
  }
}

int CameraManager::getGridIndex(float lat, float lon) const {
  int latCell = static_cast<int>((lat + 90.0f) / GRID_SIZE_DEG);
  int lonCell = static_cast<int>((lon + 180.0f) / GRID_SIZE_DEG);
  
  latCell = constrain(latCell, 0, GRID_LAT_CELLS - 1);
  lonCell = constrain(lonCell, 0, GRID_LON_CELLS - 1);
  
  return latCell * GRID_LON_CELLS + lonCell;
}

float CameraManager::haversineDistance(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0f;  // Earth radius in meters
  
  float dLat = (lat2 - lat1) * PI / 180.0f;
  float dLon = (lon2 - lon1) * PI / 180.0f;
  
  float a = sin(dLat/2) * sin(dLat/2) +
            cos(lat1 * PI / 180.0f) * cos(lat2 * PI / 180.0f) *
            sin(dLon/2) * sin(dLon/2);
  
  float c = 2 * atan2(sqrt(a), sqrt(1-a));
  
  return R * c;
}

float CameraManager::calculateBearing(float lat1, float lon1, float lat2, float lon2) {
  float dLon = (lon2 - lon1) * PI / 180.0f;
  float lat1Rad = lat1 * PI / 180.0f;
  float lat2Rad = lat2 * PI / 180.0f;
  
  float x = sin(dLon) * cos(lat2Rad);
  float y = cos(lat1Rad) * sin(lat2Rad) - sin(lat1Rad) * cos(lat2Rad) * cos(dLon);
  
  float bearing = atan2(x, y) * 180.0f / PI;
  
  // Normalize to 0-360
  if (bearing < 0) bearing += 360.0f;
  
  return bearing;
}

bool CameraManager::isHeadingTowards(float heading, float bearing, float tolerance) {
  float diff = fabs(heading - bearing);
  if (diff > 180.0f) diff = 360.0f - diff;
  return diff <= tolerance;
}

bool CameraManager::hasNearbyCamera(float lat, float lon, float radius_m) const {
  const auto& queryList = getQueryCameras();
  if (queryList.empty()) return false;
  
  // Quick bounding box check first
  // At 45° latitude, 1° ≈ 111km lat, 78km lon
  float latDelta = radius_m / 111000.0f;
  float lonDelta = radius_m / (111000.0f * cos(lat * PI / 180.0f));
  
  for (const auto& cam : queryList) {
    // Quick box check
    if (fabs(cam.latitude - lat) > latDelta) continue;
    if (fabs(cam.longitude - lon) > lonDelta) continue;
    
    // Type filter
    if (!isTypeEnabled(cam.getCameraType())) continue;
    
    // Precise distance check
    float dist = haversineDistance(lat, lon, cam.latitude, cam.longitude);
    if (dist <= radius_m) {
      return true;
    }
  }
  
  return false;
}

bool CameraManager::getClosestCamera(
  float lat, float lon,
  float heading_deg,
  float radius_m,
  NearbyCameraResult& result
) const {
  const auto& queryList = getQueryCameras();
  if (queryList.empty()) return false;
  
  float closestDist = radius_m + 1.0f;
  bool found = false;
  
  // Quick bounding box
  float latDelta = radius_m / 111000.0f;
  float lonDelta = radius_m / (111000.0f * cos(lat * PI / 180.0f));
  
  for (const auto& cam : queryList) {
    // Quick box check
    if (fabs(cam.latitude - lat) > latDelta) continue;
    if (fabs(cam.longitude - lon) > lonDelta) continue;
    
    // Type filter
    if (!isTypeEnabled(cam.getCameraType())) continue;
    
    // Precise distance
    float dist = haversineDistance(lat, lon, cam.latitude, cam.longitude);
    if (dist > radius_m) continue;
    
    // Prioritize cameras we're heading towards
    float bearing = calculateBearing(lat, lon, cam.latitude, cam.longitude);
    bool approaching = isHeadingTowards(heading_deg, bearing, 60.0f);
    
    // Prefer closer cameras and those we're approaching
    float effectiveDist = approaching ? dist * 0.5f : dist;
    
    if (effectiveDist < closestDist) {
      closestDist = effectiveDist;
      result.camera = cam;
      result.distance_m = dist;
      result.bearing_deg = bearing;
      result.isApproaching = approaching;
      found = true;
    }
  }
  
  return found;
}

std::vector<NearbyCameraResult> CameraManager::findNearby(
  float lat, float lon,
  float heading_deg,
  float radius_m,
  size_t maxResults
) const {
  std::vector<NearbyCameraResult> results;
  const auto& queryList = getQueryCameras();
  if (queryList.empty()) return results;
  
  // Quick bounding box
  float latDelta = radius_m / 111000.0f;
  float lonDelta = radius_m / (111000.0f * cos(lat * PI / 180.0f));
  
  for (const auto& cam : queryList) {
    // Quick box check
    if (fabs(cam.latitude - lat) > latDelta) continue;
    if (fabs(cam.longitude - lon) > lonDelta) continue;
    
    // Type filter
    if (!isTypeEnabled(cam.getCameraType())) continue;
    
    // Precise distance
    float dist = haversineDistance(lat, lon, cam.latitude, cam.longitude);
    if (dist > radius_m) continue;
    
    NearbyCameraResult r;
    r.camera = cam;
    r.distance_m = dist;
    r.bearing_deg = calculateBearing(lat, lon, cam.latitude, cam.longitude);
    r.isApproaching = isHeadingTowards(heading_deg, r.bearing_deg, 60.0f);
    
    results.push_back(r);
  }
  
  // Sort by distance (approaching cameras get priority)
  std::sort(results.begin(), results.end(), [](const NearbyCameraResult& a, const NearbyCameraResult& b) {
    // Approaching cameras are prioritized
    if (a.isApproaching != b.isApproaching) {
      return a.isApproaching;
    }
    return a.distance_m < b.distance_m;
  });
  
  // Limit results
  if (results.size() > maxResults) {
    results.resize(maxResults);
  }
  
  return results;
}

void CameraManager::setEnabledTypes(bool redLight, bool speed, bool alpr) {
  enableRedLight = redLight;
  enableSpeed = speed;
  enableALPR = alpr;
}

bool CameraManager::isTypeEnabled(CameraType type) const {
  switch (type) {
    case CameraType::RedLightCamera:
    case CameraType::RedLightAndSpeed:
      return enableRedLight;
    case CameraType::SpeedCamera:
      return enableSpeed;
    case CameraType::ALPR:
      return enableALPR;
    default:
      return true;
  }
}

size_t CameraManager::getRedLightCount() const {
  const auto& queryList = getQueryCameras();
  size_t count = 0;
  for (const auto& cam : queryList) {
    if (cam.type == static_cast<uint8_t>(CameraType::RedLightCamera) ||
        cam.type == static_cast<uint8_t>(CameraType::RedLightAndSpeed)) {
      count++;
    }
  }
  return count;
}

size_t CameraManager::getSpeedCameraCount() const {
  const auto& queryList = getQueryCameras();
  size_t count = 0;
  for (const auto& cam : queryList) {
    if (cam.type == static_cast<uint8_t>(CameraType::SpeedCamera) ||
        cam.type == static_cast<uint8_t>(CameraType::RedLightAndSpeed)) {
      count++;
    }
  }
  return count;
}

size_t CameraManager::getALPRCount() const {
  const auto& queryList = getQueryCameras();
  size_t count = 0;
  for (const auto& cam : queryList) {
    if (cam.type == static_cast<uint8_t>(CameraType::ALPR)) {
      count++;
    }
  }
  return count;
}

// =========================================================================
// Background Loading Implementation
// =========================================================================

// Thread-safe camera count access
size_t CameraManager::getLoadedCount() const {
  if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    size_t count = cameras.size();
    xSemaphoreGive(cameraMutex);
    return count;
  }
  return cameras.size();  // Fall back to unsynchronized if mutex fails
}

// Static task entry point - calls instance method
void CameraManager::loadTaskEntry(void* param) {
  CameraManager* self = static_cast<CameraManager*>(param);
  
  Serial.println("[Camera] Background load task started");
  
  // Perform the incremental database load
  bool success = self->loadDatabaseIncremental();
  
  // Mark loading complete
  self->backgroundLoading = false;
  self->loadProgressPercent = success ? 100 : 0;
  
  Serial.printf("[Camera] Background load task complete: %s (%d cameras)\n",
                success ? "success" : "failed", self->cameras.size());
  
  // Delete the task (task deletes itself when done)
  self->loadTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// Start background loading - returns immediately
bool CameraManager::startBackgroundLoad() {
  if (!fs) {
    Serial.println("[Camera] Cannot start background load - no filesystem");
    return false;
  }
  
  if (backgroundLoading) {
    Serial.println("[Camera] Background load already in progress");
    return false;
  }
  
  // Mark loading as in progress
  backgroundLoading = true;
  loadProgressPercent = 0;
  loadTaskShouldExit = false;
  
  // Create background task with low priority (1)
  // Lower than BLE (5) and display (2) to avoid interfering with UI
  BaseType_t result = xTaskCreate(
    loadTaskEntry,          // Task function
    "CamLoad",              // Task name
    4096,                   // Stack size (bytes)
    this,                   // Parameter (this pointer)
    1,                      // Priority (low)
    &loadTaskHandle         // Task handle
  );
  
  if (result != pdPASS) {
    Serial.println("[Camera] Failed to create background load task");
    backgroundLoading = false;
    return false;
  }
  
  Serial.println("[Camera] Background load task created");
  return true;
}

// Stop background loading (if in progress)
void CameraManager::stopBackgroundLoad() {
  if (!backgroundLoading || !loadTaskHandle) {
    return;
  }
  
  Serial.println("[Camera] Stopping background load...");
  loadTaskShouldExit = true;
  
  // Wait for task to exit (up to 2 seconds)
  for (int i = 0; i < 20 && loadTaskHandle; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  // Force delete if still running
  if (loadTaskHandle) {
    vTaskDelete(loadTaskHandle);
    loadTaskHandle = nullptr;
    Serial.println("[Camera] Background load task forcefully terminated");
  }
  
  backgroundLoading = false;
}

// Incremental database load with yielding - called from background task
bool CameraManager::loadDatabaseIncremental() {
  // List of database files to load (in order)
  struct DatabaseFile {
    const char* path;
    bool exists;
    size_t approxLines;  // Estimate for progress calculation
  };
  
  DatabaseFile files[] = {
    {"/alpr.json", false, 70000},       // Main ALPR database
    {"/redlight_cam.json", false, 200},
    {"/speed_cam.json", false, 1200}
  };
  const int numFiles = sizeof(files) / sizeof(files[0]);
  
  // Check which files exist and count total
  size_t totalLines = 0;
  int existingFiles = 0;
  for (int i = 0; i < numFiles; i++) {
    if (fs->exists(files[i].path)) {
      files[i].exists = true;
      totalLines += files[i].approxLines;
      existingFiles++;
    }
  }
  
  if (existingFiles == 0) {
    Serial.println("[Camera] No database files found for background load");
    return false;
  }
  
  Serial.printf("[Camera] Background loading %d files (~%d records)...\n", 
                existingFiles, totalLines);
  
  uint32_t overallStart = millis();
  size_t linesLoaded = 0;
  bool firstFile = true;
  
  for (int fileIdx = 0; fileIdx < numFiles; fileIdx++) {
    if (loadTaskShouldExit) {
      Serial.println("[Camera] Background load cancelled");
      return false;
    }
    
    if (!files[fileIdx].exists) continue;
    
    const char* path = files[fileIdx].path;
    
    File file = fs->open(path, "r");
    if (!file) {
      Serial.printf("[Camera] Failed to open %s\n", path);
      continue;
    }
    
    uint32_t fileStart = millis();
    size_t fileRecords = 0;
    size_t parseErrors = 0;
    char lineBuffer[256];
    size_t bufIdx = 0;
    
    // Clear cameras only for first file
    if (firstFile) {
      if (cameraMutex && xSemaphoreTake(cameraMutex, portMAX_DELAY) == pdTRUE) {
        cameras.clear();
        cameras.reserve(totalLines + 1000);
        xSemaphoreGive(cameraMutex);
      }
      firstFile = false;
    }
    
    while (file.available()) {
      // Check for cancellation every 100 lines
      if ((linesLoaded % 100) == 0) {
        if (loadTaskShouldExit) {
          file.close();
          Serial.println("[Camera] Background load cancelled mid-file");
          return false;
        }
        
        // Update progress
        loadProgressPercent = (totalLines > 0) ? 
                              (linesLoaded * 100 / totalLines) : 0;
        
        // Yield to other tasks (allow BLE, display, etc. to run)
        vTaskDelay(pdMS_TO_TICKS(1));  // 1ms yield
      }
      
      char c = file.read();
      
      if (c == '\n' || c == '\r') {
        if (bufIdx > 0) {
          lineBuffer[bufIdx] = '\0';
          
          CameraRecord record;
          if (parseCameraLine(lineBuffer, record)) {
            // Thread-safe add to camera vector
            if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
              cameras.push_back(record);
              xSemaphoreGive(cameraMutex);
              fileRecords++;
            }
          } else {
            parseErrors++;
          }
          
          linesLoaded++;
          bufIdx = 0;
        }
      } else if (bufIdx < sizeof(lineBuffer) - 1) {
        lineBuffer[bufIdx++] = c;
      }
    }
    
    // Handle last line without newline
    if (bufIdx > 0) {
      lineBuffer[bufIdx] = '\0';
      CameraRecord record;
      if (parseCameraLine(lineBuffer, record)) {
        if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          cameras.push_back(record);
          xSemaphoreGive(cameraMutex);
          fileRecords++;
        }
      }
      linesLoaded++;
    }
    
    file.close();
    
    uint32_t fileElapsed = millis() - fileStart;
    Serial.printf("[Camera] Loaded %d from %s in %dms (bg)\n", 
                  fileRecords, path, fileElapsed);
  }
  
  // Build spatial index after all files loaded
  buildSpatialIndex();
  
  loadProgressPercent = 100;
  
  uint32_t totalElapsed = millis() - overallStart;
  Serial.printf("[Camera] Background load complete: %d cameras in %dms\n",
                cameras.size(), totalElapsed);
  
  return !cameras.empty();
}
