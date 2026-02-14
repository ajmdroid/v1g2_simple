#pragma once

#include <stdint.h>

#include "camera_index.h"

struct CameraDataLoaderStatus {
    uint32_t loadAttempts = 0;
    uint32_t loadFailures = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t lastSuccessMs = 0;
};

// M1 placeholder for camera dataset loading.
// M2 will add VCAM file validation + index population.
class CameraDataLoader {
public:
    void reset();
    bool loadDefault(CameraIndex& index, uint32_t nowMs);
    CameraDataLoaderStatus status() const { return status_; }

private:
    CameraDataLoaderStatus status_ = {};
};

