#pragma once

#include <stdint.h>
#include <stddef.h>

/// Result of a road snap query.
struct RoadSnapResult {
    int32_t  latE5     = 0;      // Snapped latitude (E5)
    int32_t  lonE5     = 0;      // Snapped longitude (E5)
    uint16_t headingDeg = 0xFFFF; // Road bearing at snap point (0-359, 0xFFFF=invalid)
    uint16_t distanceCm = 0xFFFF; // Distance from query to snap point (cm, capped at 0xFFFE)
    uint8_t  roadClass = 0xFF;   // 0=motorway, 1=trunk, 2=primary, 0xFF=none
    uint8_t  speedMph  = 0;      // Speed limit (mph), 0=unknown
    bool     oneway    = false;  // True if road is one-way (bearing == travel direction)
    bool     valid     = false;  // True if a road was found within snap radius
};

/// Result of a camera proximity query.
struct CameraResult {
    int32_t  latE5      = 0;      // Camera latitude (E5)
    int32_t  lonE5      = 0;      // Camera longitude (E5)
    uint16_t bearing    = 0xFFFF; // Camera bearing (0-359, 0xFFFF=unknown)
    uint8_t  flags      = 0;     // Camera type flags
    uint8_t  speedMph   = 0;     // Speed limit at camera (mph), 0=unknown
    uint16_t distanceCm = 0xFFFF; // Distance from query to camera (cm)
    bool     valid      = false; // True if a camera was found within radius
};

/// On-disk camera record (12 bytes, little-endian).
struct __attribute__((packed)) CameraRecord {
    int32_t  latE5;              // Camera latitude (E5)
    int32_t  lonE5;              // Camera longitude (E5)
    uint16_t bearing;            // Bearing (0-359, 0xFFFF=unknown)
    uint8_t  flags;              // Camera type flags
    uint8_t  speedMph;           // Speed limit (mph), 0=unknown
};
static_assert(sizeof(CameraRecord) == 12, "CameraRecord must be 12 bytes");

/// On-disk header for road_map.bin (64 bytes, little-endian).
struct __attribute__((packed)) RoadMapHeader {
    char     magic[4];           // "RMAP"
    uint8_t  version;            // 2
    uint8_t  flags;
    uint8_t  roadClassCount;
    uint8_t  reserved;
    int32_t  minLatE5;
    int32_t  maxLatE5;
    int32_t  minLonE5;
    int32_t  maxLonE5;
    uint16_t gridRows;
    uint16_t gridCols;
    int32_t  cellSizeE5;
    uint32_t totalSegments;
    uint32_t totalPoints;
    uint16_t toleranceCm;
    uint16_t reserved2;
    uint32_t gridIndexOffset;
    uint32_t segDataOffset;
    uint32_t fileSize;
    uint32_t cameraIndexOffset;  // 0 = no cameras
    uint32_t cameraCount;        // Total camera records
};
static_assert(sizeof(RoadMapHeader) == 64, "RoadMapHeader must be 64 bytes");

/// Grid index entry (8 bytes each).
struct __attribute__((packed)) RoadMapGridEntry {
    uint32_t dataOffset;   // Byte offset from segDataOffset
    uint16_t segCount;     // Number of segments in this cell
    uint16_t reserved;
};
static_assert(sizeof(RoadMapGridEntry) == 8, "RoadMapGridEntry must be 8 bytes");

/// Reads road_map.bin entirely into PSRAM at boot.
/// All runtime queries are pure PSRAM pointer math — zero SD, zero DMA.
///
/// Thread safety: begin() at boot (single-threaded), snapToRoad() read-only.
/// Priority: boot-time Tier 7 (one-shot SD read before BLE/WiFi start).
class RoadMapReader {
public:
    /// Load road_map.bin from SD into PSRAM. Graceful no-op if file absent
    /// or PSRAM alloc fails (feature silently disabled).
    /// Must be called after storageManager.begin(), before BLE/WiFi init.
    void begin();

    /// True if road map data is loaded and ready for queries.
    bool isLoaded() const { return data_ != nullptr; }

    /// Total bytes of PSRAM consumed by the road map.
    uint32_t psramUsed() const { return fileSize_; }

    /// Number of road segments loaded.
    uint32_t segmentCount() const;

    /// Snap a lat/lon to the nearest road within snapRadiusE5.
    /// Pure PSRAM pointer math — no SD I/O, no DMA, no locks.
    /// headingDeg is returned as the raw A→B polyline bearing — caller
    /// must resolve direction ambiguity (bearing vs bearing+180) using
    /// GPS-observed travel heading, unless result.oneway is true
    /// (in which case A→B bearing IS the travel direction).
    /// If snapRadiusE5 == 0, auto-derives from the file header's
    /// RDP tolerance (1.5× toleranceCm / 111).
    /// Returns result.valid == true if a road was found.
    RoadSnapResult snapToRoad(int32_t latE5, int32_t lonE5,
                              uint16_t snapRadiusE5 = 0) const;

    /// Find the nearest ALPR camera within searchRadiusE5.
    /// Pure PSRAM pointer math — no SD I/O, no DMA, no locks.
    /// If searchRadiusE5 == 0, defaults to ~1 km (~900 E5).
    /// Returns result.valid == true if a camera was found.
    CameraResult nearestCamera(int32_t latE5, int32_t lonE5,
                               uint16_t searchRadiusE5 = 0) const;

    /// Number of cameras loaded (0 if no camera section).
    uint32_t cameraCount() const;

private:
    uint8_t* data_ = nullptr;       // PSRAM buffer (entire file)
    uint32_t fileSize_ = 0;
    uint16_t defaultSnapRadiusE5_ = 135; // Derived from header tolerance in begin()

    // Convenience pointers into data_ (set during begin)
    const RoadMapHeader*    header_    = nullptr;
    const RoadMapGridEntry* gridIndex_ = nullptr;
    const uint8_t*          segData_   = nullptr;

    // Camera section pointers (null if no cameras in file)
    const RoadMapGridEntry* camGridIndex_ = nullptr;
    const CameraRecord*     camData_      = nullptr;

    // Internal: point-to-segment distance in metres, with cos(lat) correction.
    // Applies cosLat scaling to longitude deltas so the projection is
    // geometrically correct at any latitude.
    // Returns distance in approximate metres and the nearest E5 point.
    static float pointToSegmentMetres(int32_t px, int32_t py,
                                      int32_t ax, int32_t ay,
                                      int32_t bx, int32_t by,
                                      float cosLat,
                                      int32_t& nearX, int32_t& nearY);

    // Internal: compute bearing from point A to point B in degrees (0-359).
    static uint16_t bearingDeg(int32_t aLatE5, int32_t aLonE5,
                               int32_t bLatE5, int32_t bLonE5);
};

/// Global instance.
extern RoadMapReader roadMapReader;
