#pragma once

#include <stddef.h>
#include <stdint.h>

struct CameraRecord {
    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    float snapLatitudeDeg = 0.0f;
    float snapLongitudeDeg = 0.0f;
    int16_t bearingTenthsDeg = -1;
    uint8_t widthM = 0;
    uint8_t toleranceDeg = 0;
    uint8_t type = 0;
    uint8_t speedLimit = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    uint32_t cellKey = 0;
};
static_assert(sizeof(CameraRecord) == 28, "CameraRecord layout must remain stable");

struct CameraCellSpan {
    uint32_t cellKey = 0;
    uint32_t beginIndex = 0;
    uint32_t endIndex = 0;
};
static_assert(sizeof(CameraCellSpan) == 12, "CameraCellSpan layout must remain compact");

struct CameraIndexOwnedBuffers {
    CameraRecord* records = nullptr;
    uint32_t recordCount = 0;
    CameraCellSpan* spans = nullptr;
    uint32_t spanCount = 0;
    uint32_t version = 0;
};

struct CameraIndexStats {
    bool loaded = false;
    uint32_t cameraCount = 0;
    uint32_t bucketCount = 0;
    uint32_t version = 0;
};

class CameraIndex {
public:
    static constexpr float kCellSizeDeg = 0.01f;
    static constexpr uint32_t kRawScanCap = 128;
    // ALPR-only mode requires a larger span table; keep it in PSRAM.
    static constexpr uint32_t kSpanBudgetBytes = 512u * 1024u;

    CameraIndex() = default;
    ~CameraIndex();
    CameraIndex(const CameraIndex&) = delete;
    CameraIndex& operator=(const CameraIndex&) = delete;
    CameraIndex(CameraIndex&& other) noexcept;
    CameraIndex& operator=(CameraIndex&& other) noexcept;

    void clear();
    bool adopt(CameraIndexOwnedBuffers&& buffers);
    CameraIndexOwnedBuffers release();
    static void freeOwnedBuffers(CameraIndexOwnedBuffers& buffers);

    bool isLoaded() const { return loaded_; }
    uint32_t cameraCount() const { return buffers_.recordCount; }
    uint32_t bucketCount() const { return buffers_.spanCount; }
    uint32_t version() const { return buffers_.version; }
    const CameraRecord* records() const { return buffers_.records; }
    const CameraCellSpan* spans() const { return buffers_.spans; }
    CameraIndexStats stats() const;

    static uint32_t encodeCellKeyFromCell(int32_t latitudeCell, int32_t longitudeCell);
    static uint32_t encodeCellKey(float latitudeDeg, float longitudeDeg);

private:
    bool loaded_ = false;
    CameraIndexOwnedBuffers buffers_ = {};
};
