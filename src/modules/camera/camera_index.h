#pragma once

#include <stdint.h>

struct CameraIndexStats {
    bool loaded = false;
    uint32_t cameraCount = 0;
    uint32_t bucketCount = 0;
};

// Placeholder immutable index for M1 scaffolding.
// M2 will replace the placeholder dataset methods with real index builders.
class CameraIndex {
public:
    void clear();
    bool setPlaceholderDataset(uint32_t cameraCount, uint32_t bucketCount);

    bool isLoaded() const { return loaded_; }
    uint32_t cameraCount() const { return cameraCount_; }
    uint32_t bucketCount() const { return bucketCount_; }
    CameraIndexStats stats() const;

private:
    bool loaded_ = false;
    uint32_t cameraCount_ = 0;
    uint32_t bucketCount_ = 0;
};

