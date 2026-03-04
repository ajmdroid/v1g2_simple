/**
 * road_map_reader.cpp — Load road_map.bin into PSRAM at boot.
 *
 * Option A design: entire file (~1-3 MB) is read once into PSRAM via
 * ps_malloc() during boot, before WiFi/BLE start. All runtime queries
 * are pure pointer math on the PSRAM buffer — zero SD I/O, zero DMA
 * pressure, zero locks.
 *
 * Graceful degradation: if the file is absent, too large, corrupt, or
 * PSRAM alloc fails, the feature is silently disabled (isLoaded() == false).
 */

#include "road_map_reader.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#include <FS.h>
#include "../../storage_manager.h"
#include <esp_heap_caps.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <cstring>
#include <cmath>
#include <algorithm>

RoadMapReader roadMapReader;

static constexpr const char* ROAD_MAP_PATH = "/road_map.bin";
static constexpr uint32_t MAX_FILE_SIZE = 8 * 1024 * 1024;  // 8 MB sanity cap
static constexpr uint32_t MIN_FILE_SIZE = 64;                // Header alone

void RoadMapReader::begin() {
#ifndef UNIT_TEST
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        Serial.println("[RoadMap] No SD card — road snap disabled");
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(ROAD_MAP_PATH)) {
        Serial.println("[RoadMap] No road_map.bin on SD — road snap disabled");
        return;
    }

    File f = fs->open(ROAD_MAP_PATH, "r");
    if (!f) {
        Serial.println("[RoadMap] Failed to open road_map.bin");
        return;
    }

    const uint32_t fSize = static_cast<uint32_t>(f.size());
    if (fSize < MIN_FILE_SIZE || fSize > MAX_FILE_SIZE) {
        Serial.printf("[RoadMap] Bad file size: %lu bytes (need %lu-%lu)\n",
                      static_cast<unsigned long>(fSize),
                      static_cast<unsigned long>(MIN_FILE_SIZE),
                      static_cast<unsigned long>(MAX_FILE_SIZE));
        f.close();
        return;
    }

    // Check PSRAM availability before allocation.
    const uint32_t psramFree = static_cast<uint32_t>(ESP.getFreePsram());
    const uint32_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (psramLargest < fSize) {
        Serial.printf("[RoadMap] Insufficient PSRAM: need=%lu free=%lu largest=%lu\n",
                      static_cast<unsigned long>(fSize),
                      static_cast<unsigned long>(psramFree),
                      static_cast<unsigned long>(psramLargest));
        f.close();
        return;
    }

    // Allocate from PSRAM — does NOT touch internal SRAM / DMA pool.
    uint8_t* buf = static_cast<uint8_t*>(ps_malloc(fSize));
    if (!buf) {
        Serial.printf("[RoadMap] ps_malloc(%lu) failed\n",
                      static_cast<unsigned long>(fSize));
        f.close();
        return;
    }

    // Read entire file into PSRAM buffer.
    const unsigned long readStartMs = millis();
    const size_t bytesRead = f.read(buf, fSize);
    f.close();
    const unsigned long readMs = millis() - readStartMs;

    if (bytesRead != fSize) {
        Serial.printf("[RoadMap] Short read: got %lu of %lu bytes\n",
                      static_cast<unsigned long>(bytesRead),
                      static_cast<unsigned long>(fSize));
        free(buf);
        return;
    }

    // Validate header.
    const RoadMapHeader* hdr = reinterpret_cast<const RoadMapHeader*>(buf);
    if (memcmp(hdr->magic, "RMAP", 4) != 0) {
        Serial.println("[RoadMap] Bad magic (expected RMAP)");
        free(buf);
        return;
    }
    if (hdr->version != 1 && hdr->version != 2) {
        Serial.printf("[RoadMap] Unsupported version: %u\n", hdr->version);
        free(buf);
        return;
    }
    if (hdr->fileSize != fSize) {
        Serial.printf("[RoadMap] Header fileSize mismatch: header=%lu actual=%lu\n",
                      static_cast<unsigned long>(hdr->fileSize),
                      static_cast<unsigned long>(fSize));
        free(buf);
        return;
    }

    // Validate grid index bounds.
    const uint32_t gridSize = static_cast<uint32_t>(hdr->gridRows) * hdr->gridCols * 8;
    if (hdr->gridIndexOffset + gridSize > fSize) {
        Serial.println("[RoadMap] Grid index extends past end of file");
        free(buf);
        return;
    }
    if (hdr->segDataOffset > fSize) {
        Serial.println("[RoadMap] Segment data offset past end of file");
        free(buf);
        return;
    }

    // All good — commit pointers.
    data_ = buf;
    fileSize_ = fSize;
    header_ = hdr;
    gridIndex_ = reinterpret_cast<const RoadMapGridEntry*>(buf + hdr->gridIndexOffset);
    segData_ = buf + hdr->segDataOffset;

    // Parse camera section if present (cameraIndexOffset != 0).
    if (hdr->cameraIndexOffset > 0 && hdr->cameraCount > 0) {
        const uint32_t camGridSize =
            static_cast<uint32_t>(hdr->gridRows) * hdr->gridCols * sizeof(RoadMapGridEntry);
        const uint32_t camDataOffset = hdr->cameraIndexOffset + camGridSize;
        const uint32_t camDataEnd =
            camDataOffset + hdr->cameraCount * sizeof(CameraRecord);
        if (camDataEnd <= fSize) {
            camGridIndex_ = reinterpret_cast<const RoadMapGridEntry*>(
                buf + hdr->cameraIndexOffset);
            camData_ = reinterpret_cast<const CameraRecord*>(
                buf + camDataOffset);
        } else {
            Serial.println("[RoadMap] Camera section extends past EOF — skipping cameras");
        }
    }

    // Derive snap radius from the RDP tolerance stored in the header.
    // 1.5× tolerance so we don't miss roads simplified away from their
    // true position.  toleranceCm → metres → E5 (1 E5 ≈ 1.11 m).
    if (hdr->toleranceCm > 0) {
        const float tolMetres = static_cast<float>(hdr->toleranceCm) / 100.0f;
        const float radiusM = tolMetres * 1.5f;
        const float radiusE5f = radiusM / 1.11f;
        defaultSnapRadiusE5_ = static_cast<uint16_t>(
            std::min(radiusE5f, 65534.0f));
    }

    Serial.printf("[RoadMap] Loaded %lu bytes into PSRAM in %lu ms "
                  "(segs=%lu pts=%lu cams=%lu grid=%ux%u cell=%.2f° snapR=%uE5)\n",
                  static_cast<unsigned long>(fSize),
                  static_cast<unsigned long>(readMs),
                  static_cast<unsigned long>(hdr->totalSegments),
                  static_cast<unsigned long>(hdr->totalPoints),
                  static_cast<unsigned long>(hdr->cameraCount),
                  static_cast<unsigned>(hdr->gridRows),
                  static_cast<unsigned>(hdr->gridCols),
                  static_cast<float>(hdr->cellSizeE5) / 100000.0f,
                  static_cast<unsigned>(defaultSnapRadiusE5_));
#endif  // UNIT_TEST
}

uint32_t RoadMapReader::segmentCount() const {
    return header_ ? header_->totalSegments : 0;
}

uint32_t RoadMapReader::cameraCount() const {
    return header_ ? header_->cameraCount : 0;
}

// ---------------------------------------------------------------------------
// Geometry helpers — all pure math, no I/O
// ---------------------------------------------------------------------------

float RoadMapReader::pointToSegmentMetres(int32_t px, int32_t py,
                                           int32_t ax, int32_t ay,
                                           int32_t bx, int32_t by,
                                           float cosLat,
                                           int32_t& nearX, int32_t& nearY) {
    // Point-to-segment distance with cos(lat) correction.
    // Lat deltas are in E5 (1 E5 ≈ 1.11 m). Lon deltas must be scaled
    // by cos(latitude) to get true east-west distance.
    // We work in a local metre-like frame for the projection, then
    // convert the nearest point back to E5 coords.
    static constexpr float E5_TO_METRES = 1.11f;  // 1 E5 unit ≈ 1.11 m (lat)

    // Scale to approximate metres for projection.
    const float axM = static_cast<float>(ax) * E5_TO_METRES;
    const float ayM = static_cast<float>(ay) * E5_TO_METRES * cosLat;
    const float bxM = static_cast<float>(bx) * E5_TO_METRES;
    const float byM = static_cast<float>(by) * E5_TO_METRES * cosLat;
    const float pxM = static_cast<float>(px) * E5_TO_METRES;
    const float pyM = static_cast<float>(py) * E5_TO_METRES * cosLat;

    const float dx = bxM - axM;
    const float dy = byM - ayM;
    const float lenSq = dx * dx + dy * dy;

    float t;
    if (lenSq < 0.01f) {
        // Degenerate segment (endpoints identical).
        t = 0.0f;
    } else {
        t = ((pxM - axM) * dx + (pyM - ayM) * dy) / lenSq;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }

    // Nearest point in E5 coords (interpolate in original coord space).
    nearX = ax + static_cast<int32_t>(t * static_cast<float>(bx - ax));
    nearY = ay + static_cast<int32_t>(t * static_cast<float>(by - ay));

    // Distance in metres.
    const float nxM = axM + t * dx;
    const float nyM = ayM + t * dy;
    const float ex = pxM - nxM;
    const float ey = pyM - nyM;
    return sqrtf(ex * ex + ey * ey);
}

uint16_t RoadMapReader::bearingDeg(int32_t aLatE5, int32_t aLonE5,
                                    int32_t bLatE5, int32_t bLonE5) {
    // Approximate bearing using E5 deltas.
    // At mid-latitudes cos(lat) ≈ 0.7, but for bearing direction the
    // ratio matters more than absolute distance. Good enough for road heading.
    const float dLat = static_cast<float>(bLatE5 - aLatE5);
    const float dLon = static_cast<float>(bLonE5 - aLonE5);

    // Apply rough cos(lat) correction for longitude.
    const float midLatRad = static_cast<float>(aLatE5 + bLatE5) / 2.0f
                            / 100000.0f * (3.14159265f / 180.0f);
    const float cosLat = cosf(midLatRad);
    const float dLonCorrected = dLon * cosLat;

    float deg = atan2f(dLonCorrected, dLat) * (180.0f / 3.14159265f);
    if (deg < 0.0f) deg += 360.0f;
    return static_cast<uint16_t>(deg) % 360;
}

// ---------------------------------------------------------------------------
// snapToRoad — the main query, pure PSRAM pointer math
// ---------------------------------------------------------------------------

RoadSnapResult RoadMapReader::snapToRoad(int32_t latE5, int32_t lonE5,
                                          uint16_t snapRadiusE5) const {
    RoadSnapResult result;
    if (!data_ || !header_ || !gridIndex_ || !segData_) {
        return result;
    }

    // Auto-derive snap radius from header tolerance when caller passes 0.
    if (snapRadiusE5 == 0) {
        snapRadiusE5 = defaultSnapRadiusE5_;
    }

    const RoadMapHeader& h = *header_;

    // Check if query is within bounding box (with one cell margin).
    if (latE5 < h.minLatE5 - h.cellSizeE5 || latE5 > h.maxLatE5 + h.cellSizeE5 ||
        lonE5 < h.minLonE5 - h.cellSizeE5 || lonE5 > h.maxLonE5 + h.cellSizeE5) {
        return result;
    }

    // Precompute cos(lat) once for all distance calculations.
    const float latRad = static_cast<float>(latE5) / 100000.0f * (3.14159265f / 180.0f);
    const float cosLat = cosf(latRad);

    // Convert snap radius from E5 to metres (1 E5 ≈ 1.11 m along latitude).
    const float snapRadiusMetres = static_cast<float>(snapRadiusE5) * 1.11f;

    // Compute grid cell for query point.
    const int centerRow = (latE5 - h.minLatE5) / h.cellSizeE5;
    const int centerCol = (lonE5 - h.minLonE5) / h.cellSizeE5;

    float bestDistM = snapRadiusMetres;
    int32_t bestNearLat = 0;
    int32_t bestNearLon = 0;
    // Track the segment edge endpoints nearest to the snap point for heading.
    int32_t bestSegALat = 0, bestSegALon = 0;
    int32_t bestSegBLat = 0, bestSegBLon = 0;
    uint8_t bestRoadClass = 0xFF;
    uint8_t bestSpeedMph = 0;
    bool bestOneway = false;

    // v2 segments have a trailing speed byte (1 extra byte per segment).
    const bool isV2 = (h.version >= 2);

    // Search 3x3 neighbourhood.
    for (int dr = -1; dr <= 1; ++dr) {
        const int row = centerRow + dr;
        if (row < 0 || row >= static_cast<int>(h.gridRows)) continue;

        for (int dc = -1; dc <= 1; ++dc) {
            const int col = centerCol + dc;
            if (col < 0 || col >= static_cast<int>(h.gridCols)) continue;

            const uint32_t cellIdx = static_cast<uint32_t>(row) * h.gridCols
                                     + static_cast<uint32_t>(col);
            const RoadMapGridEntry& cell = gridIndex_[cellIdx];
            if (cell.segCount == 0) continue;

            // Walk segments in this cell.
            const uint8_t* ptr = segData_ + cell.dataOffset;
            for (uint16_t s = 0; s < cell.segCount; ++s) {
                // Parse segment header (variable-length record).
                const uint8_t roadClass = ptr[0];
                const uint8_t segFlags = ptr[1];
                const bool oneway = (segFlags & 0x01) != 0;
                const uint16_t pointCount = *reinterpret_cast<const uint16_t*>(ptr + 2);
                const int32_t lat0 = *reinterpret_cast<const int32_t*>(ptr + 4);
                const int32_t lon0 = *reinterpret_cast<const int32_t*>(ptr + 8);

                if (pointCount < 2) {
                    // Skip degenerate segment.
                    const uint32_t segSize = 12 + (pointCount > 1 ? (pointCount - 1) * 4 : 0)
                                             + (isV2 ? 1 : 0);
                    ptr += segSize;
                    continue;
                }

                // Walk the polyline edges.
                int32_t prevLat = lat0;
                int32_t prevLon = lon0;
                const int16_t* deltas = reinterpret_cast<const int16_t*>(ptr + 12);

                for (uint16_t p = 1; p < pointCount; ++p) {
                    const int32_t curLat = prevLat + deltas[(p - 1) * 2];
                    const int32_t curLon = prevLon + deltas[(p - 1) * 2 + 1];

                    int32_t nearLat, nearLon;
                    const float dist = pointToSegmentMetres(
                        latE5, lonE5,
                        prevLat, prevLon,
                        curLat, curLon,
                        cosLat,
                        nearLat, nearLon);

                    if (dist < bestDistM) {
                        bestDistM = dist;
                        bestNearLat = nearLat;
                        bestNearLon = nearLon;
                        bestSegALat = prevLat;
                        bestSegALon = prevLon;
                        bestSegBLat = curLat;
                        bestSegBLon = curLon;
                        bestRoadClass = roadClass;
                        bestOneway = oneway;
                        // Read speed byte from end of this segment (v2 only).
                        if (isV2) {
                            bestSpeedMph = ptr[12 + (pointCount - 1) * 4];
                        }
                    }

                    prevLat = curLat;
                    prevLon = curLon;
                }

                // Advance pointer to next segment.
                ptr += 12 + (pointCount - 1) * 4 + (isV2 ? 1 : 0);
            }
        }
    }

    if (bestRoadClass != 0xFF) {
        result.valid = true;
        result.latE5 = bestNearLat;
        result.lonE5 = bestNearLon;
        result.roadClass = bestRoadClass;
        result.speedMph = bestSpeedMph;
        result.oneway = bestOneway;
        // Raw A→B bearing of the matched edge. For one-way roads this IS
        // the travel direction. For two-way roads, caller must resolve
        // ambiguity (bearing vs bearing+180) using GPS heading.
        result.headingDeg = bearingDeg(bestSegALat, bestSegALon,
                                        bestSegBLat, bestSegBLon);

        // Distance is already in metres — convert to cm.
        const float distCm = bestDistM * 100.0f;
        result.distanceCm = (distCm > 65534.0f) ? 0xFFFE
                            : static_cast<uint16_t>(distCm);
    }

    return result;
}

// ---------------------------------------------------------------------------
// nearestCamera — spatial query over camera grid, pure PSRAM pointer math
// ---------------------------------------------------------------------------

CameraResult RoadMapReader::nearestCamera(int32_t latE5, int32_t lonE5,
                                           uint16_t searchRadiusE5) const {
    CameraResult result;
    if (!data_ || !header_ || !camGridIndex_ || !camData_) {
        return result;
    }

    // Default search radius: ~1 km ≈ 900 E5
    if (searchRadiusE5 == 0) {
        searchRadiusE5 = 900;
    }

    const RoadMapHeader& h = *header_;

    // Bounds check (with one cell margin).
    if (latE5 < h.minLatE5 - h.cellSizeE5 || latE5 > h.maxLatE5 + h.cellSizeE5 ||
        lonE5 < h.minLonE5 - h.cellSizeE5 || lonE5 > h.maxLonE5 + h.cellSizeE5) {
        return result;
    }

    // cos(lat) for longitude correction.
    const float latRad = static_cast<float>(latE5) / 100000.0f * (3.14159265f / 180.0f);
    const float cosLat = cosf(latRad);
    static constexpr float E5_TO_METRES = 1.11f;

    const float searchRadiusM = static_cast<float>(searchRadiusE5) * E5_TO_METRES;

    // Grid cell for query point.
    const int centerRow = (latE5 - h.minLatE5) / h.cellSizeE5;
    const int centerCol = (lonE5 - h.minLonE5) / h.cellSizeE5;

    float bestDistM = searchRadiusM;
    const CameraRecord* bestCam = nullptr;

    // Search 3×3 neighbourhood.
    for (int dr = -1; dr <= 1; ++dr) {
        const int row = centerRow + dr;
        if (row < 0 || row >= static_cast<int>(h.gridRows)) continue;

        for (int dc = -1; dc <= 1; ++dc) {
            const int col = centerCol + dc;
            if (col < 0 || col >= static_cast<int>(h.gridCols)) continue;

            const uint32_t cellIdx = static_cast<uint32_t>(row) * h.gridCols
                                     + static_cast<uint32_t>(col);
            const RoadMapGridEntry& cell = camGridIndex_[cellIdx];
            if (cell.segCount == 0) continue;  // segCount re-used as camCount

            // Camera records for this cell.
            const CameraRecord* cams = reinterpret_cast<const CameraRecord*>(
                reinterpret_cast<const uint8_t*>(camData_) + cell.dataOffset);

            for (uint16_t i = 0; i < cell.segCount; ++i) {
                const CameraRecord& cam = cams[i];
                const float dLat = static_cast<float>(cam.latE5 - latE5) * E5_TO_METRES;
                const float dLon = static_cast<float>(cam.lonE5 - lonE5) * E5_TO_METRES * cosLat;
                const float dist = sqrtf(dLat * dLat + dLon * dLon);
                if (dist < bestDistM) {
                    bestDistM = dist;
                    bestCam = &cam;
                }
            }
        }
    }

    if (bestCam) {
        result.valid = true;
        result.latE5 = bestCam->latE5;
        result.lonE5 = bestCam->lonE5;
        result.bearing = bestCam->bearing;
        result.flags = bestCam->flags;
        result.speedMph = bestCam->speedMph;
        const float distCm = bestDistM * 100.0f;
        result.distanceCm = (distCm > 65534.0f) ? 0xFFFE
                            : static_cast<uint16_t>(distCm);
    }

    return result;
}
