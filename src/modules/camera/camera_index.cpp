#include "camera_index.h"

void CameraIndex::clear() {
    loaded_ = false;
    cameraCount_ = 0;
    bucketCount_ = 0;
}

bool CameraIndex::setPlaceholderDataset(uint32_t cameraCount, uint32_t bucketCount) {
    if (cameraCount == 0 || bucketCount == 0) {
        clear();
        return false;
    }
    loaded_ = true;
    cameraCount_ = cameraCount;
    bucketCount_ = bucketCount;
    return true;
}

CameraIndexStats CameraIndex::stats() const {
    CameraIndexStats out;
    out.loaded = loaded_;
    out.cameraCount = cameraCount_;
    out.bucketCount = bucketCount_;
    return out;
}

