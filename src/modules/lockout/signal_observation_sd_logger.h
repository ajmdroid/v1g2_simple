#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdint.h>

#include <atomic>

#include "../../psram_freertos_alloc.h"
#include "signal_observation_log.h"

struct SignalObservationSdStats {
    bool enabled = false;
    uint32_t enqueued = 0;
    uint32_t queueDrops = 0;
    uint32_t deduped = 0;
    uint32_t written = 0;
    uint32_t writeFail = 0;
    uint32_t rotations = 0;
};

// Asynchronous SD persistence for lockout candidate observations.
// Uses a low-priority Core 0 writer task so capture paths remain non-blocking.
class SignalObservationSdLogger {
public:
    void setBootId(uint32_t id);
    void begin(bool sdAvailable);
    bool enqueue(const SignalObservation& observation);

    bool isEnabled() const { return enabled_; }
    const char* csvPath() const { return csvPathBuf_; }
    SignalObservationSdStats stats() const;

private:
    struct DedupeBucket {
        SignalObservation observation = {};
        bool valid = false;
    };

    static void writerTaskEntry(void* param);
    void writerTaskLoop();
    bool appendObservation(const SignalObservation& observation);
    bool ensureLockoutDir(fs::FS& fs);
    bool ensureCsvHeader(File& file);
    bool rotateIfNeeded(fs::FS& fs);

    static bool sameBucket(const SignalObservation& a, const SignalObservation& b);
    bool shouldDedupe(const SignalObservation& observation, size_t* matchedBucketIndex) const;
    void rememberPersistedObservation(const SignalObservation& observation, size_t matchedBucketIndex);
    void resetDedupeState();

    bool enabled_ = false;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t writerTask_ = nullptr;
    PsramQueueAllocation queueAllocation_ = {};
    bool queueInPsram_ = false;
    bool writerTaskStackInPsram_ = false;
    bool dirReady_ = false;
    bool headerReady_ = false;
    uint32_t bootId_ = 0;
    char csvPathBuf_[80] = {0};
    static constexpr size_t kDedupeBucketCount = 16;
    DedupeBucket dedupeBuckets_[kDedupeBucketCount] = {};
    size_t nextDedupeBucketIndex_ = 0;

    std::atomic<uint32_t> enqueued_{0};
    std::atomic<uint32_t> queueDrops_{0};
    std::atomic<uint32_t> deduped_{0};
    std::atomic<uint32_t> written_{0};
    std::atomic<uint32_t> writeFail_{0};
    std::atomic<uint32_t> rotations_{0};

#ifdef UNIT_TEST
public:
    bool rotateIfNeededForTest(fs::FS& fs) { return rotateIfNeeded(fs); }
#endif
};

extern SignalObservationSdLogger signalObservationSdLogger;
