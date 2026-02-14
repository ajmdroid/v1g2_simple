#include "camera_index.h"

#include <algorithm>
#include <cmath>
#include <esp_heap_caps.h>
#include <utility>

namespace {
constexpr int32_t kLatitudeCellOffset = 9000;
constexpr int32_t kLongitudeCellOffset = 18000;
}  // namespace

CameraIndex::~CameraIndex() {
    clear();
}

CameraIndex::CameraIndex(CameraIndex&& other) noexcept {
    *this = std::move(other);
}

CameraIndex& CameraIndex::operator=(CameraIndex&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    clear();
    loaded_ = other.loaded_;
    buffers_ = other.buffers_;
    other.loaded_ = false;
    other.buffers_ = {};
    return *this;
}

void CameraIndex::clear() {
    freeOwnedBuffers(buffers_);
    loaded_ = false;
}

CameraIndexStats CameraIndex::stats() const {
    CameraIndexStats out;
    out.loaded = loaded_;
    out.cameraCount = buffers_.recordCount;
    out.bucketCount = buffers_.spanCount;
    out.version = buffers_.version;
    return out;
}

bool CameraIndex::adopt(CameraIndexOwnedBuffers&& buffers) {
    if (!buffers.records || buffers.recordCount == 0) {
        freeOwnedBuffers(buffers);
        clear();
        return false;
    }
    if (buffers.spanCount > 0 && !buffers.spans) {
        freeOwnedBuffers(buffers);
        clear();
        return false;
    }

    clear();
    buffers_ = buffers;
    loaded_ = true;
    buffers = {};
    return true;
}

CameraIndexOwnedBuffers CameraIndex::release() {
    CameraIndexOwnedBuffers out = buffers_;
    buffers_ = {};
    loaded_ = false;
    return out;
}

void CameraIndex::freeOwnedBuffers(CameraIndexOwnedBuffers& buffers) {
    if (buffers.records) {
        heap_caps_free(buffers.records);
        buffers.records = nullptr;
    }
    if (buffers.spans) {
        heap_caps_free(buffers.spans);
        buffers.spans = nullptr;
    }
    buffers.recordCount = 0;
    buffers.spanCount = 0;
    buffers.version = 0;
}

uint32_t CameraIndex::encodeCellKey(float latitudeDeg, float longitudeDeg) {
    if (!std::isfinite(latitudeDeg) || !std::isfinite(longitudeDeg)) {
        return 0;
    }

    const int32_t latCellRaw = static_cast<int32_t>(std::floor(latitudeDeg / kCellSizeDeg));
    const int32_t lonCellRaw = static_cast<int32_t>(std::floor(longitudeDeg / kCellSizeDeg));
    const int32_t latCell = std::clamp(latCellRaw, -kLatitudeCellOffset, kLatitudeCellOffset);
    const int32_t lonCell = std::clamp(lonCellRaw, -kLongitudeCellOffset, kLongitudeCellOffset);

    const uint32_t latEncoded = static_cast<uint32_t>(latCell + kLatitudeCellOffset);
    const uint32_t lonEncoded = static_cast<uint32_t>(lonCell + kLongitudeCellOffset);
    return (latEncoded << 16) | lonEncoded;
}
