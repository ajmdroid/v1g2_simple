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
    if (hdr->version != 1) {
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

    Serial.printf("[RoadMap] Loaded %lu bytes into PSRAM in %lu ms "
                  "(segs=%lu pts=%lu grid=%ux%u cell=%.2f°)\n",
                  static_cast<unsigned long>(fSize),
                  static_cast<unsigned long>(readMs),
                  static_cast<unsigned long>(hdr->totalSegments),
                  static_cast<unsigned long>(hdr->totalPoints),
                  static_cast<unsigned>(hdr->gridRows),
                  static_cast<unsigned>(hdr->gridCols),
                  static_cast<float>(hdr->cellSizeE5) / 100000.0f);
#endif  // UNIT_TEST
}

uint32_t RoadMapReader::segmentCount() const {
    return header_ ? header_->totalSegments : 0;
}

// ---------------------------------------------------------------------------
// Geometry helpers — all pure math, no I/O
// ---------------------------------------------------------------------------

float RoadMapReader::pointToSegmentDistE5(int32_t px, int32_t py,
                                           int32_t ax, int32_t ay,
                                           int32_t bx, int32_t by,
                                           int32_t& nearX, int32_t& nearY) {
    // Point-to-segment distance in E5 coordinate space.
    // px/py = query point, ax/ay..bx/by = segment endpoints.
    const float dx = static_cast<float>(bx - ax);
    const float dy = static_cast<float>(by - ay);
    const float lenSq = dx * dx + dy * dy;

    float t;
    if (lenSq < 1.0f) {
        // Degenerate segment (endpoints identical).
        t = 0.0f;
    } else {
        t = (static_cast<float>(px - ax) * dx +
             static_cast<float>(py - ay) * dy) / lenSq;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }

    nearX = ax + static_cast<int32_t>(t * dx);
    nearY = ay + static_cast<int32_t>(t * dy);

    const float ex = static_cast<float>(px - nearX);
    const float ey = static_cast<float>(py - nearY);
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

    const RoadMapHeader& h = *header_;

    // Check if query is within bounding box (with one cell margin).
    if (latE5 < h.minLatE5 - h.cellSizeE5 || latE5 > h.maxLatE5 + h.cellSizeE5 ||
        lonE5 < h.minLonE5 - h.cellSizeE5 || lonE5 > h.maxLonE5 + h.cellSizeE5) {
        return result;
    }

    // Compute grid cell for query point.
    const int centerRow = (latE5 - h.minLatE5) / h.cellSizeE5;
    const int centerCol = (lonE5 - h.minLonE5) / h.cellSizeE5;

    float bestDistE5 = static_cast<float>(snapRadiusE5);
    int32_t bestNearLat = 0;
    int32_t bestNearLon = 0;
    // Track the segment endpoints nearest to the snap point for heading.
    int32_t bestSegALat = 0, bestSegALon = 0;
    int32_t bestSegBLat = 0, bestSegBLon = 0;
    uint8_t bestRoadClass = 0xFF;

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
                // ptr[1] = flags (unused in snap)
                const uint16_t pointCount = *reinterpret_cast<const uint16_t*>(ptr + 2);
                const int32_t lat0 = *reinterpret_cast<const int32_t*>(ptr + 4);
                const int32_t lon0 = *reinterpret_cast<const int32_t*>(ptr + 8);

                if (pointCount < 2) {
                    // Skip degenerate segment.
                    ptr += 12 + (pointCount > 1 ? (pointCount - 1) * 4 : 0);
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
                    const float dist = pointToSegmentDistE5(
                        latE5, lonE5,
                        prevLat, prevLon,
                        curLat, curLon,
                        nearLat, nearLon);

                    if (dist < bestDistE5) {
                        bestDistE5 = dist;
                        bestNearLat = nearLat;
                        bestNearLon = nearLon;
                        bestSegALat = prevLat;
                        bestSegALon = prevLon;
                        bestSegBLat = curLat;
                        bestSegBLon = curLon;
                        bestRoadClass = roadClass;
                    }

                    prevLat = curLat;
                    prevLon = curLon;
                }

                // Advance pointer to next segment.
                ptr += 12 + (pointCount - 1) * 4;
            }
        }
    }

    if (bestRoadClass != 0xFF) {
        result.valid = true;
        result.latE5 = bestNearLat;
        result.lonE5 = bestNearLon;
        result.roadClass = bestRoadClass;
        result.headingDeg = bearingDeg(bestSegALat, bestSegALon,
                                        bestSegBLat, bestSegBLon);

        // Convert E5 distance to approximate centimeters.
        // 1 E5 unit ≈ 1.11 m at equator (lat), so distCm ≈ dist * 111.
        // Good enough for a "within 50m" check.
        const float distCm = bestDistE5 * 111.0f;
        result.distanceCm = (distCm > 65534.0f) ? 0xFFFE
                            : static_cast<uint16_t>(distCm);
    }

    return result;
}
