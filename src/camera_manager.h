// Camera Alert Manager
// Loads and queries red light camera / speed camera / ALPR databases
// Data format: NDJSON (newline-delimited JSON) compatible with RDForum ExCam format

#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Camera types (matches ExCam flg values)
enum class CameraType : uint8_t {
  RedLightAndSpeed = 1,  // flg=1: Red light camera with speed enforcement
  SpeedCamera = 2,       // flg=2: Speed camera only
  RedLightCamera = 3,    // flg=3: Red light camera only
  ALPR = 4,              // Custom: Automatic License Plate Reader
  Unknown = 0
};

// Compact camera record (~20 bytes per camera)
struct CameraRecord {
  float latitude;
  float longitude;
  uint8_t type;           // CameraType
  uint8_t speedLimit;     // 0 if unknown, in display units
  uint8_t directionCount; // 0-2 directions
  uint16_t directions[2]; // Up to 2 directions (0-359 degrees)
  bool isMetric;          // true = kmh, false = mph
  
  CameraType getCameraType() const { return static_cast<CameraType>(type); }
  
  // Get type name for display
  const char* getTypeName() const {
    switch (getCameraType()) {
      case CameraType::RedLightAndSpeed: return "RLC+SPD";
      case CameraType::SpeedCamera: return "SPEED";
      case CameraType::RedLightCamera: return "REDLIGHT";
      case CameraType::ALPR: return "ALPR";
      default: return "CAM";
    }
  }
  
  // Get short type name for display (3 chars max)
  const char* getShortTypeName() const {
    switch (getCameraType()) {
      case CameraType::RedLightAndSpeed: return "RLS";
      case CameraType::SpeedCamera: return "SPD";
      case CameraType::RedLightCamera: return "RLC";
      case CameraType::ALPR: return "LPR";
      default: return "CAM";
    }
  }
};

// Result for nearby camera queries
struct NearbyCameraResult {
  CameraRecord camera;
  float distance_m;     // Distance in meters
  float bearing_deg;    // Bearing to camera (0-359)
  bool isApproaching;   // True if heading towards camera
};

// Grid cell for spatial indexing
struct GridCell {
  uint16_t startIndex;
  uint16_t count;
};

class CameraManager {
public:
  CameraManager();
  ~CameraManager();
  
  // Initialize and load database from SD card
  // Returns true if database loaded successfully
  bool begin(fs::FS* filesystem);
  
  // Load camera database from file (NDJSON format)
  // Supports: V1 ExCam JSON format, POI Factory CSV (future)
  // append: if true, add to existing cameras; if false, clear first
  bool loadDatabase(const char* path, bool append = false);
  
  // Clear all loaded cameras
  void clear();
  
  // Get database info
  size_t getCameraCount() const { return cameras.size(); }
  size_t getRegionalCacheCount() const { return regionalCache.size(); }
  const char* getDatabaseName() const { return databaseName; }
  const char* getDatabaseDate() const { return databaseDate; }
  bool isLoaded() const { return !cameras.empty() || !regionalCache.empty(); }
  bool hasRegionalCache() const { return !regionalCache.empty(); }
  
  // Regional caching - keeps only cameras within radius of GPS position
  // Call when GPS gets first fix or periodically when user moves significantly
  bool buildRegionalCache(float lat, float lon, float radiusMiles = 100.0f);
  bool needsCacheRefresh(float lat, float lon, float distanceThresholdMiles = 50.0f) const;
  bool saveRegionalCache(fs::FS* filesystem, const char* path = "/cameras_cache.json");
  bool loadRegionalCache(fs::FS* filesystem, const char* path = "/cameras_cache.json");
  void getCacheCenter(float& lat, float& lon) const { lat = cacheCenterLat; lon = cacheCenterLon; }
  float getCacheRadius() const { return cacheRadiusMi; }
  
  // Find cameras within radius of current position
  // Returns cameras sorted by distance (closest first)
  std::vector<NearbyCameraResult> findNearby(
    float lat, float lon, 
    float heading_deg,
    float radius_m = 1000.0f,
    size_t maxResults = 5
  ) const;
  
  // Quick check if any camera is within alert range
  // More efficient than findNearby for frequent polling
  bool hasNearbyCamera(float lat, float lon, float radius_m = 500.0f) const;
  
  // Get the closest camera (null if none in range)
  bool getClosestCamera(
    float lat, float lon,
    float heading_deg,
    float radius_m,
    NearbyCameraResult& result
  ) const;
  
  // Filter settings
  void setEnabledTypes(bool redLight, bool speed, bool alpr);
  bool isTypeEnabled(CameraType type) const;
  
  // Statistics
  size_t getRedLightCount() const;
  size_t getSpeedCameraCount() const;
  size_t getALPRCount() const;

  // Utility functions (public for external use)
  static float haversineDistance(float lat1, float lon1, float lat2, float lon2);
  static float calculateBearing(float lat1, float lon1, float lat2, float lon2);
  
  // Background loading - non-blocking database load
  // Call setFilesystem() first, then startBackgroundLoad()
  // Load regional cache first for instant alerts, then background load full DB
  void setFilesystem(fs::FS* filesystem) { fs = filesystem; }
  fs::FS* getFilesystem() const { return fs; }
  bool startBackgroundLoad();
  void stopBackgroundLoad();
  bool isBackgroundLoading() const { return backgroundLoading; }
  int getLoadProgress() const { return loadProgressPercent; }  // 0-100
  size_t getLoadedCount() const;  // Thread-safe camera count

private:
  fs::FS* fs = nullptr;
  std::vector<CameraRecord> cameras;          // Full database from SD
  std::vector<CameraRecord> regionalCache;    // Subset near GPS position
  
  // Thread safety for camera vector (modified by background task, read by queries)
  mutable SemaphoreHandle_t cameraMutex = nullptr;
  
  // Background loading state
  TaskHandle_t loadTaskHandle = nullptr;
  volatile bool backgroundLoading = false;
  volatile bool loadTaskShouldExit = false;
  volatile int loadProgressPercent = 0;
  static void loadTaskEntry(void* param);
  bool loadDatabaseIncremental();  // Called by background task
  
  // Regional cache metadata
  float cacheCenterLat = 0.0f;
  float cacheCenterLon = 0.0f;
  float cacheRadiusMi = 0.0f;
  uint32_t cacheBuiltMs = 0;
  
  // Database metadata
  char databaseName[64] = {0};
  char databaseDate[16] = {0};
  
  // Type filters
  bool enableRedLight = true;
  bool enableSpeed = true;
  bool enableALPR = true;
  
  // Spatial index (grid-based for memory efficiency)
  static constexpr float GRID_SIZE_DEG = 0.1f;  // ~11km cells
  static constexpr int GRID_LAT_CELLS = 180 * 10;  // 1800 cells
  static constexpr int GRID_LON_CELLS = 360 * 10;  // 3600 cells
  std::vector<GridCell> spatialIndex;
  bool indexBuilt = false;
  
  // Build spatial index for fast queries
  void buildSpatialIndex();
  
  // Get grid cell index for a coordinate
  int getGridIndex(float lat, float lon) const;
  
  // Parse a single NDJSON line
  bool parseCameraLine(const char* line, CameraRecord& record);
  
  // Get cameras to query (regional cache if available, else full database)
  const std::vector<CameraRecord>& getQueryCameras() const;
  
  // Check if heading is towards camera (within tolerance)
  static bool isHeadingTowards(float heading, float bearing, float tolerance = 45.0f);
};

// Global instance
extern CameraManager cameraManager;

#endif // CAMERA_MANAGER_H
