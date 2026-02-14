#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#include "camera_index.h"

struct CameraDataLoaderStatus {
    uint32_t loadAttempts = 0;
    uint32_t loadFailures = 0;
    uint32_t loadSkipsMemoryGuard = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t lastSuccessMs = 0;
    uint32_t lastLoadDurationMs = 0;
    uint32_t maxLoadDurationMs = 0;
    uint32_t lastSortDurationMs = 0;
    uint32_t lastSpanBuildDurationMs = 0;
    uint32_t lastInternalFree = 0;
    uint32_t lastInternalLargestBlock = 0;
    uint32_t memoryGuardMinFree = 0;
    uint32_t memoryGuardMinLargestBlock = 0;
    bool taskRunning = false;
    bool loadInProgress = false;
    bool reloadPending = false;
    uint32_t readyVersion = 0;
};

class CameraDataLoader {
public:
    static constexpr uint32_t kChunkRecords = 1024;
    static constexpr uint32_t kLoaderTaskStackSize = 8192;
    static constexpr UBaseType_t kLoaderTaskPriority = 1;

    void begin();
    void reset();
    void requestReload();
    bool consumeReady(CameraIndex& activeIndex);
    CameraDataLoaderStatus status() const;

private:
    enum class BuildOutcome : uint8_t {
        Success = 0,
        Failed = 1,
        SkippedMemoryGuard = 2,
    };

    struct VcamHeader {
        char magic[4];
        uint32_t version;
        uint32_t count;
        uint32_t recordSize;
    };

    struct RawVcamRecord {
        float latitudeDeg;
        float longitudeDeg;
        float snapLatitudeDeg;
        float snapLongitudeDeg;
        int16_t bearingTenthsDeg;
        uint8_t widthM;
        uint8_t toleranceDeg;
        uint8_t type;
        uint8_t speedLimit;
        uint8_t flags;
        uint8_t reserved;
    };

    static_assert(sizeof(VcamHeader) == 16, "VCAM header must be 16 bytes");
    static_assert(sizeof(RawVcamRecord) == 24, "VCAM record must be 24 bytes");

    static void loaderTaskEntry(void* param);
    void loaderTaskLoop();
    BuildOutcome buildEnforcementIndex(CameraIndexOwnedBuffers& outBuffers);
    bool accumulateRecordCount(uint32_t& outTotalRecords);
    bool loadRecords(CameraRecord* outRecords, uint32_t totalRecords);
    bool loadFileRecords(const char* path,
                         CameraRecord* outRecords,
                         uint32_t totalRecords,
                         uint32_t& writeIndex,
                         RawVcamRecord* chunkBuffer,
                         uint32_t chunkCapacity);
    bool buildSpans(CameraRecord* records,
                    uint32_t recordCount,
                    CameraCellSpan*& outSpans,
                    uint32_t& outSpanCount);
    void publishReadyBuffers(CameraIndexOwnedBuffers& built);
    void clearReadyBuffers();

    static constexpr const char* kDatasetFiles[2] = {"/speed_cam.bin", "/redlight_cam.bin"};
    static constexpr uint32_t kExpectedVersion = 1;
    static constexpr uint32_t kExpectedRecordSize = 24;
    static constexpr uint32_t kPsramHeadroomBytes = 256u * 1024u;
    // Must stay above WiFi AP+STA runtime threshold (20 KiB) + margin.
    // Previous value of 24 KiB was too tight — WiFi would shut down.
    static constexpr uint32_t kMemoryGuardMinFreeInternal = 32768;     // 32 KiB
    static constexpr uint32_t kMemoryGuardMinLargestBlock = 16384;     // 16 KiB

    TaskHandle_t loaderTask_ = nullptr;
    std::atomic<bool> reloadPending_{false};
    std::atomic<bool> loadInProgress_{false};
    std::atomic<uint32_t> nextVersion_{1};
    portMUX_TYPE readyMux_ = portMUX_INITIALIZER_UNLOCKED;
    mutable portMUX_TYPE statusMux_ = portMUX_INITIALIZER_UNLOCKED;
    CameraIndexOwnedBuffers readyBuffers_ = {};
    CameraDataLoaderStatus status_ = {};
};
