#include "camera_data_loader.h"

void CameraDataLoader::reset() {
    status_ = {};
}

bool CameraDataLoader::loadDefault(CameraIndex& index, uint32_t nowMs) {
    status_.loadAttempts++;
    status_.lastAttemptMs = nowMs;

    // M1 scaffold intentionally does not perform filesystem IO yet.
    index.clear();
    status_.loadFailures++;
    return false;
}

