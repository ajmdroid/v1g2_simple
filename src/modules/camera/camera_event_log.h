#pragma once

#include <stddef.h>
#include <stdint.h>

struct CameraEvent {
    uint32_t tsMs = 0;
    uint32_t cameraId = 0;
    uint16_t distanceM = 0;
    uint8_t type = 0;
    bool synthetic = false;
};

struct CameraEventLogStats {
    uint32_t published = 0;
    uint32_t drops = 0;
    size_t size = 0;
};

class CameraEventLog {
public:
    static constexpr size_t kCapacity = 64;

    void reset();
    bool publish(const CameraEvent& event);
    size_t copyRecent(CameraEvent* out, size_t maxCount) const;
    CameraEventLogStats stats() const;

private:
    static uint8_t nextIndex(uint8_t idx) {
        return static_cast<uint8_t>((idx + 1u) % kCapacity);
    }
    static uint8_t prevIndex(uint8_t idx) {
        return static_cast<uint8_t>((idx + kCapacity - 1u) % kCapacity);
    }

    CameraEvent ring_[kCapacity] = {};
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
    uint8_t count_ = 0;
    uint32_t published_ = 0;
    uint32_t drops_ = 0;
};

extern CameraEventLog cameraEventLog;
