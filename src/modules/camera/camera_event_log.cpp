#include "camera_event_log.h"

#include <algorithm>

CameraEventLog cameraEventLog;

void CameraEventLog::reset() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    published_ = 0;
    drops_ = 0;
}

bool CameraEventLog::publish(const CameraEvent& event) {
    bool dropped = false;

    if (count_ == kCapacity) {
        tail_ = nextIndex(tail_);
        count_--;
        drops_++;
        dropped = true;
    }

    ring_[head_] = event;
    head_ = nextIndex(head_);
    count_++;
    published_++;
    return !dropped;
}

size_t CameraEventLog::copyRecent(CameraEvent* out, size_t maxCount) const {
    if (!out || maxCount == 0 || count_ == 0) {
        return 0;
    }

    const size_t copyCount = std::min<size_t>(maxCount, count_);
    uint8_t idx = head_;
    for (size_t i = 0; i < copyCount; ++i) {
        idx = prevIndex(idx);
        out[i] = ring_[idx];
    }
    return copyCount;
}

CameraEventLogStats CameraEventLog::stats() const {
    CameraEventLogStats out;
    out.published = published_;
    out.drops = drops_;
    out.size = count_;
    return out;
}
