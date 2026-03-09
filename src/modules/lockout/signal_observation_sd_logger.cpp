#include "signal_observation_sd_logger.h"

#include "../../storage_manager.h"
#include "../../perf_metrics.h"

#include <FS.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr const char* LOCKOUT_DIR_PATH = "/lockout";
constexpr const char* LOCKOUT_CSV_PATH_FALLBACK = "/lockout/lockout_candidates.csv";
constexpr const char* LOCKOUT_CSV_HEADER =
    "tsMs,bandRaw,frequencyMHz,strength,hasFix,fixAgeMs,satellites,hdopX10,locationValid,latitudeE5,longitudeE5\n";
constexpr UBaseType_t LOCKOUT_SD_QUEUE_DEPTH = 64;
constexpr uint32_t LOCKOUT_SD_WRITER_STACK_SIZE = 6144;constexpr UBaseType_t LOCKOUT_SD_WRITER_PRIORITY = 1;
constexpr uint32_t LOCKOUT_SD_DEDUPE_MIN_REPEAT_MS = 15000;
constexpr uint16_t LOCKOUT_SD_FREQ_TOL_MHZ = 5;
constexpr uint8_t LOCKOUT_SD_STRENGTH_TOL = 1;
constexpr int32_t LOCKOUT_SD_LOCATION_TOL_E5 = 25;
constexpr size_t LOCKOUT_SD_MAX_FILE_BYTES = 2 * 1024 * 1024;  // 2MB active file cap.

void buildLockoutCsvPath(uint32_t bootId, char* out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    if (bootId == 0) {
        snprintf(out, outLen, "%s", LOCKOUT_CSV_PATH_FALLBACK);
        return;
    }
    snprintf(out, outLen, "/lockout/lockout_candidates_boot_%lu.csv",
             static_cast<unsigned long>(bootId));
}
}  // namespace

SignalObservationSdLogger signalObservationSdLogger;

void SignalObservationSdLogger::setBootId(uint32_t id) {
    bootId_ = id;
    buildLockoutCsvPath(bootId_, csvPathBuf_, sizeof(csvPathBuf_));
    headerReady_ = false;
}

void SignalObservationSdLogger::begin(bool sdAvailable) {
    enabled_ = false;
    if (!sdAvailable) {
        return;
    }

    if (csvPathBuf_[0] == '\0') {
        setBootId(bootId_);
    }

    dirReady_ = false;
    headerReady_ = false;
    resetDedupeState();

    if (!queue_) {
        queue_ = createQueuePreferPsram(LOCKOUT_SD_QUEUE_DEPTH,
                                        sizeof(SignalObservation),
                                        queueAllocation_,
                                        &queueInPsram_);
        if (!queue_) {
            Serial.println("[LockoutSD] ERROR: Failed to create queue");
            return;
        }
        if (!queueInPsram_) {
            Serial.println("[LockoutSD] WARN: queue using internal SRAM fallback");
        }
    }

    if (!writerTask_) {
        BaseType_t rc = xTaskCreatePinnedToCore(
            writerTaskEntry,
            "LockoutSdWriter",
            LOCKOUT_SD_WRITER_STACK_SIZE,
            this,
            LOCKOUT_SD_WRITER_PRIORITY,
            &writerTask_,
            0);
        if (rc != pdPASS) {
            Serial.println("[LockoutSD] ERROR: Failed to create writer task");
            return;
        }
    }

    enabled_ = true;
}

bool SignalObservationSdLogger::enqueue(const SignalObservation& observation) {
    if (!enabled_ || !queue_) {
        return false;
    }
    if (xQueueSend(queue_, &observation, 0) != pdTRUE) {
        queueDrops_.fetch_add(1, std::memory_order_relaxed);
        PERF_INC(sigObsQueueDrops);
        return false;
    }
    enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

SignalObservationSdStats SignalObservationSdLogger::stats() const {
    SignalObservationSdStats out;
    out.enabled = enabled_;
    out.enqueued = enqueued_.load(std::memory_order_relaxed);
    out.queueDrops = queueDrops_.load(std::memory_order_relaxed);
    out.deduped = deduped_.load(std::memory_order_relaxed);
    out.written = written_.load(std::memory_order_relaxed);
    out.writeFail = writeFail_.load(std::memory_order_relaxed);
    out.rotations = rotations_.load(std::memory_order_relaxed);
    return out;
}

void SignalObservationSdLogger::writerTaskEntry(void* param) {
    SignalObservationSdLogger* self = static_cast<SignalObservationSdLogger*>(param);
    self->writerTaskLoop();
}

void SignalObservationSdLogger::writerTaskLoop() {
    while (true) {
        SignalObservation observation{};
        if (xQueueReceive(queue_, &observation, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        (void)appendObservation(observation);
        taskYIELD();
    }
}

bool SignalObservationSdLogger::sameBucket(const SignalObservation& a,
                                           const SignalObservation& b) {
    if (a.bandRaw != b.bandRaw) {
        return false;
    }

    const int freqDiff = abs(static_cast<int>(a.frequencyMHz) - static_cast<int>(b.frequencyMHz));
    if (freqDiff > LOCKOUT_SD_FREQ_TOL_MHZ) {
        return false;
    }

    const int strengthDiff = abs(static_cast<int>(a.strength) - static_cast<int>(b.strength));
    if (strengthDiff > LOCKOUT_SD_STRENGTH_TOL) {
        return false;
    }

    if (a.locationValid != b.locationValid) {
        return false;
    }
    if (!a.locationValid) {
        return true;
    }

    const int32_t latDiff = abs(a.latitudeE5 - b.latitudeE5);
    const int32_t lonDiff = abs(a.longitudeE5 - b.longitudeE5);
    return latDiff <= LOCKOUT_SD_LOCATION_TOL_E5 && lonDiff <= LOCKOUT_SD_LOCATION_TOL_E5;
}

bool SignalObservationSdLogger::shouldDedupe(const SignalObservation& observation,
                                             size_t* matchedBucketIndex) const {
    if (matchedBucketIndex) {
        *matchedBucketIndex = kDedupeBucketCount;
    }
    for (size_t i = 0; i < kDedupeBucketCount; ++i) {
        const DedupeBucket& bucket = dedupeBuckets_[i];
        if (!bucket.valid) {
            continue;
        }
        if (!sameBucket(observation, bucket.observation)) {
            continue;
        }
        if (matchedBucketIndex) {
            *matchedBucketIndex = i;
        }
        const uint32_t elapsedMs = static_cast<uint32_t>(observation.tsMs - bucket.observation.tsMs);
        return elapsedMs < LOCKOUT_SD_DEDUPE_MIN_REPEAT_MS;
    }
    return false;
}

void SignalObservationSdLogger::rememberPersistedObservation(const SignalObservation& observation,
                                                            size_t matchedBucketIndex) {
    size_t index = matchedBucketIndex;
    if (index >= kDedupeBucketCount) {
        index = nextDedupeBucketIndex_;
        nextDedupeBucketIndex_ = (nextDedupeBucketIndex_ + 1) % kDedupeBucketCount;
    }
    dedupeBuckets_[index].observation = observation;
    dedupeBuckets_[index].valid = true;
}

void SignalObservationSdLogger::resetDedupeState() {
    for (size_t i = 0; i < kDedupeBucketCount; ++i) {
        dedupeBuckets_[i].valid = false;
    }
    nextDedupeBucketIndex_ = 0;
}

bool SignalObservationSdLogger::ensureLockoutDir(fs::FS& fs) {
    if (dirReady_) {
        return true;
    }
    if (fs.mkdir(LOCKOUT_DIR_PATH) || fs.exists(LOCKOUT_DIR_PATH)) {
        dirReady_ = true;
        return true;
    }
    return false;
}

bool SignalObservationSdLogger::ensureCsvHeader(File& file) {
    if (file.size() == 0) {
        headerReady_ = false;
    }
    if (headerReady_) {
        return true;
    }
    const size_t headerLen = strlen(LOCKOUT_CSV_HEADER);
    const size_t headerWritten =
        file.write(reinterpret_cast<const uint8_t*>(LOCKOUT_CSV_HEADER), headerLen);
    if (headerWritten != headerLen) {
        return false;
    }
    headerReady_ = true;
    return true;
}

bool SignalObservationSdLogger::rotateIfNeeded(fs::FS& fs) {
    const char* path = (csvPathBuf_[0] != '\0') ? csvPathBuf_ : LOCKOUT_CSV_PATH_FALLBACK;
    if (!fs.exists(path)) {
        return true;
    }

    File current = fs.open(path, FILE_READ);
    if (!current) {
        return true;
    }
    const size_t currentSize = current.size();
    current.close();

    if (currentSize < LOCKOUT_SD_MAX_FILE_BYTES) {
        return true;
    }

    const String prevPath = String(path) + ".prev";
    if (fs.exists(prevPath)) {
        fs.remove(prevPath);
    }
    if (!fs.rename(path, prevPath)) {
        fs.remove(path);
    }

    headerReady_ = false;
    resetDedupeState();
    rotations_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SignalObservationSdLogger::appendObservation(const SignalObservation& observation) {
    size_t matchedBucketIndex = kDedupeBucketCount;
    if (shouldDedupe(observation, &matchedBucketIndex)) {
        deduped_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Helper lambda: increment both local writeFail_ and central perf counter
    auto recordWriteFail = [this]() {
        writeFail_.fetch_add(1, std::memory_order_relaxed);
        PERF_INC(sigObsWriteFail);
    };

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        recordWriteFail();
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        recordWriteFail();
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        recordWriteFail();
        return false;
    }

    if (!ensureLockoutDir(*fs)) {
        recordWriteFail();
        return false;
    }
    if (!rotateIfNeeded(*fs)) {
        recordWriteFail();
        return false;
    }

    const char* path = (csvPathBuf_[0] != '\0') ? csvPathBuf_ : LOCKOUT_CSV_PATH_FALLBACK;
    File file = fs->open(path, FILE_APPEND, true);
    if (!file && dirReady_) {
        dirReady_ = false;
        if (ensureLockoutDir(*fs)) {
            file = fs->open(path, FILE_APPEND, true);
        }
    }
    if (!file) {
        recordWriteFail();
        return false;
    }

    if (!ensureCsvHeader(file)) {
        recordWriteFail();
        file.close();
        return false;
    }

    char line[196];
    const int n = snprintf(
        line,
        sizeof(line),
        "%lu,%u,%u,%u,%u,%lu,%u,%u,%u,%ld,%ld\n",
        static_cast<unsigned long>(observation.tsMs),
        static_cast<unsigned int>(observation.bandRaw),
        static_cast<unsigned int>(observation.frequencyMHz),
        static_cast<unsigned int>(observation.strength),
        observation.hasFix ? 1u : 0u,
        static_cast<unsigned long>(observation.fixAgeMs),
        static_cast<unsigned int>(observation.satellites),
        static_cast<unsigned int>(observation.hdopX10),
        observation.locationValid ? 1u : 0u,
        static_cast<long>(observation.latitudeE5),
        static_cast<long>(observation.longitudeE5));
    if (n <= 0 || n >= static_cast<int>(sizeof(line))) {
        recordWriteFail();
        file.close();
        return false;
    }

    const size_t lineLen = static_cast<size_t>(n);
    const size_t lineWritten = file.write(reinterpret_cast<const uint8_t*>(line), lineLen);
    file.close();
    if (lineWritten != lineLen) {
        recordWriteFail();
        return false;
    }

    written_.fetch_add(1, std::memory_order_relaxed);
    rememberPersistedObservation(observation, matchedBucketIndex);
    return true;
}
