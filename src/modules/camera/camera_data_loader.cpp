#include "camera_data_loader.h"

#include "../../storage_manager.h"

#include <Arduino.h>
#include <FS.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>
#include <utility>

namespace {
constexpr const char* kLoaderTaskName = "CameraLoader";
constexpr uint32_t kLoaderCore = 0;
}  // namespace

void CameraDataLoader::begin() {
    if (loaderTask_) {
        return;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        loaderTaskEntry,
        kLoaderTaskName,
        kLoaderTaskStackSize,
        this,
        kLoaderTaskPriority,
        &loaderTask_,
        kLoaderCore);
    if (rc != pdPASS) {
        loaderTask_ = nullptr;
        Serial.println("[CameraLoader] ERROR: Failed to create loader task");
        return;
    }

    portENTER_CRITICAL(&statusMux_);
    status_.taskRunning = true;
    portEXIT_CRITICAL(&statusMux_);

    if (reloadPending_.load(std::memory_order_relaxed)) {
        xTaskNotifyGive(loaderTask_);
    }
}

void CameraDataLoader::reset() {
    reloadPending_.store(false, std::memory_order_relaxed);
    loadInProgress_.store(false, std::memory_order_relaxed);
    nextVersion_.store(1, std::memory_order_relaxed);
    clearReadyBuffers();

    portENTER_CRITICAL(&statusMux_);
    status_ = {};
    status_.taskRunning = (loaderTask_ != nullptr);
    status_.memoryGuardMinFree = kMemoryGuardMinFreeInternal;
    status_.memoryGuardMinLargestBlock = kMemoryGuardMinLargestBlock;
    portEXIT_CRITICAL(&statusMux_);
}

void CameraDataLoader::requestReload() {
    reloadPending_.store(true, std::memory_order_relaxed);

    portENTER_CRITICAL(&statusMux_);
    status_.reloadPending = true;
    portEXIT_CRITICAL(&statusMux_);

    // Re-create task if it self-deleted after previous load.
    // This keeps the 8 KiB stack allocated only while loading.
    if (!loaderTask_) {
        begin();
    } else {
        xTaskNotifyGive(loaderTask_);
    }
}

bool CameraDataLoader::consumeReady(CameraIndex& activeIndex) {
    CameraIndexOwnedBuffers ready{};
    bool hasReady = false;

    portENTER_CRITICAL(&readyMux_);
    if (readyBuffers_.records && readyBuffers_.recordCount > 0) {
        ready = readyBuffers_;
        readyBuffers_ = {};
        hasReady = true;
    }
    portEXIT_CRITICAL(&readyMux_);

    if (!hasReady) {
        return false;
    }

    if (!activeIndex.adopt(std::move(ready))) {
        CameraIndex::freeOwnedBuffers(ready);
        portENTER_CRITICAL(&statusMux_);
        status_.loadFailures++;
        portEXIT_CRITICAL(&statusMux_);
        return false;
    }

    return true;
}

CameraDataLoaderStatus CameraDataLoader::status() const {
    CameraDataLoaderStatus out;
    portENTER_CRITICAL(&statusMux_);
    out = status_;
    portEXIT_CRITICAL(&statusMux_);
    out.taskRunning = (loaderTask_ != nullptr);
    out.loadInProgress = loadInProgress_.load(std::memory_order_relaxed);
    out.reloadPending = reloadPending_.load(std::memory_order_relaxed);
    out.memoryGuardMinFree = kMemoryGuardMinFreeInternal;
    out.memoryGuardMinLargestBlock = kMemoryGuardMinLargestBlock;
    return out;
}

void CameraDataLoader::loaderTaskEntry(void* param) {
    CameraDataLoader* self = static_cast<CameraDataLoader*>(param);
    self->loaderTaskLoop();
}

void CameraDataLoader::loaderTaskLoop() {
    // Process all pending reload requests, then self-delete to free
    // the 8 KiB internal SRAM stack.  requestReload() will re-create
    // this task on demand if another load is needed later.
    while (reloadPending_.exchange(false, std::memory_order_relaxed)) {
        const uint32_t nowMs = millis();
        loadInProgress_.store(true, std::memory_order_relaxed);

        portENTER_CRITICAL(&statusMux_);
        status_.taskRunning = true;
        status_.reloadPending = false;
        status_.loadInProgress = true;
        status_.loadAttempts++;
        status_.lastAttemptMs = nowMs;
        portEXIT_CRITICAL(&statusMux_);

        CameraIndexOwnedBuffers built{};
        const uint32_t loadStartMs = millis();
        const BuildOutcome outcome = buildRuntimeIndex(built);
        const bool ok = (outcome == BuildOutcome::Success);
        const uint32_t loadDurationMs = static_cast<uint32_t>(millis() - loadStartMs);

        if (ok) {
            const uint32_t readyVersion = built.version;
            publishReadyBuffers(built);
            portENTER_CRITICAL(&statusMux_);
            status_.lastSuccessMs = millis();
            status_.readyVersion = readyVersion;
            status_.lastLoadDurationMs = loadDurationMs;
            if (loadDurationMs > status_.maxLoadDurationMs) {
                status_.maxLoadDurationMs = loadDurationMs;
            }
            portEXIT_CRITICAL(&statusMux_);
        } else {
            CameraIndex::freeOwnedBuffers(built);
            portENTER_CRITICAL(&statusMux_);
            if (outcome == BuildOutcome::SkippedMemoryGuard) {
                status_.loadSkipsMemoryGuard++;
            } else {
                status_.loadFailures++;
            }
            status_.lastLoadDurationMs = loadDurationMs;
            if (loadDurationMs > status_.maxLoadDurationMs) {
                status_.maxLoadDurationMs = loadDurationMs;
            }
            portEXIT_CRITICAL(&statusMux_);
        }

        loadInProgress_.store(false, std::memory_order_relaxed);
        portENTER_CRITICAL(&statusMux_);
        status_.loadInProgress = false;
        portEXIT_CRITICAL(&statusMux_);
    }

    // Self-delete: free 8 KiB stack back to internal SRAM heap.
    // Avoids permanent fragmentation that starves WiFi AP+STA.
    portENTER_CRITICAL(&statusMux_);
    status_.taskRunning = false;
    portEXIT_CRITICAL(&statusMux_);
    loaderTask_ = nullptr;
    vTaskDelete(nullptr);
}

CameraDataLoader::BuildOutcome CameraDataLoader::buildRuntimeIndex(CameraIndexOwnedBuffers& outBuffers) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        Serial.println("[CameraLoader] Build failed: storage not ready or SD not mounted");
        return BuildOutcome::Failed;
    }

    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largestInternal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&statusMux_);
    status_.lastInternalFree = freeInternal;
    status_.lastInternalLargestBlock = largestInternal;
    portEXIT_CRITICAL(&statusMux_);
    if (freeInternal < kMemoryGuardMinFreeInternal || largestInternal < kMemoryGuardMinLargestBlock) {
        Serial.printf("[CameraLoader] Build skipped: internal SRAM guard (free=%lu block=%lu need>=%lu/%lu)\n",
                      static_cast<unsigned long>(freeInternal),
                      static_cast<unsigned long>(largestInternal),
                      static_cast<unsigned long>(kMemoryGuardMinFreeInternal),
                      static_cast<unsigned long>(kMemoryGuardMinLargestBlock));
        return BuildOutcome::SkippedMemoryGuard;
    }

    uint32_t totalRecords = 0;
    if (!accumulateRecordCount(totalRecords) || totalRecords == 0) {
        Serial.println("[CameraLoader] Build failed: unable to count camera records");
        return BuildOutcome::Failed;
    }

    const uint32_t requiredBytes = totalRecords * sizeof(CameraRecord);
    const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largestPsram < (requiredBytes + kPsramHeadroomBytes)) {
        Serial.printf("[CameraLoader] Build failed: PSRAM headroom (largest=%lu need=%lu + headroom=%lu)\n",
                      static_cast<unsigned long>(largestPsram),
                      static_cast<unsigned long>(requiredBytes),
                      static_cast<unsigned long>(kPsramHeadroomBytes));
        return BuildOutcome::Failed;
    }

    CameraRecord* records = static_cast<CameraRecord*>(
        heap_caps_malloc(requiredBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!records) {
        Serial.printf("[CameraLoader] Build failed: PSRAM alloc returned null (%lu bytes)\n",
                      static_cast<unsigned long>(requiredBytes));
        return BuildOutcome::Failed;
    }

    bool ok = loadRecords(records, totalRecords);
    if (!ok) {
        Serial.println("[CameraLoader] Build failed: loadRecords() failed");
        heap_caps_free(records);
        return BuildOutcome::Failed;
    }

    const uint32_t sortStartMs = millis();
    std::sort(records, records + totalRecords, [](const CameraRecord& lhs, const CameraRecord& rhs) {
        if (lhs.cellKey != rhs.cellKey) {
            return lhs.cellKey < rhs.cellKey;
        }
        if (lhs.latitudeDeg != rhs.latitudeDeg) {
            return lhs.latitudeDeg < rhs.latitudeDeg;
        }
        return lhs.longitudeDeg < rhs.longitudeDeg;
    });
    const uint32_t sortDurationMs = static_cast<uint32_t>(millis() - sortStartMs);
    portENTER_CRITICAL(&statusMux_);
    status_.lastSortDurationMs = sortDurationMs;
    portEXIT_CRITICAL(&statusMux_);
    vTaskDelay(pdMS_TO_TICKS(1));

    CameraCellSpan* spans = nullptr;
    uint32_t spanCount = 0;
    const uint32_t spanStartMs = millis();
    ok = buildSpans(records, totalRecords, spans, spanCount);
    const uint32_t spanDurationMs = static_cast<uint32_t>(millis() - spanStartMs);
    portENTER_CRITICAL(&statusMux_);
    status_.lastSpanBuildDurationMs = spanDurationMs;
    portEXIT_CRITICAL(&statusMux_);
    if (!ok) {
        Serial.println("[CameraLoader] Build failed: span table build failed");
        heap_caps_free(records);
        return BuildOutcome::Failed;
    }

    outBuffers.records = records;
    outBuffers.recordCount = totalRecords;
    outBuffers.spans = spans;
    outBuffers.spanCount = spanCount;
    outBuffers.version = nextVersion_.fetch_add(1, std::memory_order_relaxed);
    return BuildOutcome::Success;
}

bool CameraDataLoader::accumulateRecordCount(uint32_t& outTotalRecords) {
    outTotalRecords = 0;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        Serial.println("[CameraLoader] Record count failed: filesystem unavailable");
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        Serial.println("[CameraLoader] Record count failed: SD lock unavailable");
        return false;
    }

    // NOTE: This preflight pass counts records; file contents are read in loadFileRecords().
    // Keeping it separate preserves simple allocation checks before bulk decode.
    for (const char* path : kDatasetFiles) {
        File file = fs->open(path, FILE_READ);
        if (!file) {
            Serial.printf("[CameraLoader] Record count failed: open %s\n", path);
            return false;
        }

        VcamHeader header{};
        const size_t headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
        const bool validHeader =
            std::memcmp(header.magic, "VCAM", 4) == 0 &&
            header.version == kExpectedVersion &&
            header.recordSize == kExpectedRecordSize;
        if (headerRead != sizeof(header) || !validHeader) {
            Serial.printf("[CameraLoader] Record count failed: invalid header in %s\n", path);
            file.close();
            return false;
        }

        const size_t expectedBytes = sizeof(VcamHeader) + (header.count * header.recordSize);
        if (file.size() < expectedBytes) {
            Serial.printf("[CameraLoader] Record count failed: truncated %s (size=%lu expected>=%lu)\n",
                          path,
                          static_cast<unsigned long>(file.size()),
                          static_cast<unsigned long>(expectedBytes));
            file.close();
            return false;
        }

        outTotalRecords += header.count;
        file.close();
    }

    return outTotalRecords > 0;
}

bool CameraDataLoader::loadRecords(CameraRecord* outRecords, uint32_t totalRecords) {
    if (!outRecords || totalRecords == 0) {
        return false;
    }

    // PSRAM-only: do NOT fall back to internal SRAM — 24 KiB from internal
    // would push WiFi below its runtime SRAM threshold and cause shutdown.
    RawVcamRecord* chunkBuffer = static_cast<RawVcamRecord*>(
        heap_caps_malloc(sizeof(RawVcamRecord) * kChunkRecords, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!chunkBuffer) {
        Serial.printf("[CameraLoader] Build failed: chunk buffer alloc returned null (%lu bytes)\n",
                      static_cast<unsigned long>(sizeof(RawVcamRecord) * kChunkRecords));
        return false;
    }

    uint32_t writeIndex = 0;
    for (const char* path : kDatasetFiles) {
        if (!loadFileRecords(path,
                             outRecords,
                             totalRecords,
                             writeIndex,
                             chunkBuffer,
                             kChunkRecords)) {
            heap_caps_free(chunkBuffer);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    heap_caps_free(chunkBuffer);
    return writeIndex == totalRecords;
}

bool CameraDataLoader::loadFileRecords(const char* path,
                                       CameraRecord* outRecords,
                                       uint32_t totalRecords,
                                       uint32_t& writeIndex,
                                       RawVcamRecord* chunkBuffer,
                                       uint32_t chunkCapacity) {
    if (!path || !outRecords || !chunkBuffer || chunkCapacity == 0) {
        Serial.println("[CameraLoader] loadFileRecords failed: invalid arguments");
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        Serial.println("[CameraLoader] loadFileRecords failed: filesystem unavailable");
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        Serial.println("[CameraLoader] loadFileRecords failed: SD lock unavailable");
        return false;
    }

    File file = fs->open(path, FILE_READ);
    if (!file) {
        Serial.printf("[CameraLoader] loadFileRecords failed: open %s\n", path);
        return false;
    }

    VcamHeader header{};
    const size_t headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    const bool validHeader =
        std::memcmp(header.magic, "VCAM", 4) == 0 &&
        header.version == kExpectedVersion &&
        header.recordSize == kExpectedRecordSize;
    if (headerRead != sizeof(header) || !validHeader) {
        Serial.printf("[CameraLoader] loadFileRecords failed: invalid header in %s\n", path);
        file.close();
        return false;
    }

    uint32_t remaining = header.count;
    while (remaining > 0) {
        const uint32_t toRead = std::min<uint32_t>(chunkCapacity, remaining);
        const size_t bytesToRead = static_cast<size_t>(toRead) * sizeof(RawVcamRecord);
        const size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(chunkBuffer), bytesToRead);
        if (bytesRead != bytesToRead) {
            Serial.printf("[CameraLoader] loadFileRecords failed: short read in %s (%lu/%lu)\n",
                          path,
                          static_cast<unsigned long>(bytesRead),
                          static_cast<unsigned long>(bytesToRead));
            file.close();
            return false;
        }

        for (uint32_t i = 0; i < toRead; ++i) {
            if (writeIndex >= totalRecords) {
                Serial.printf("[CameraLoader] loadFileRecords failed: write overflow in %s\n", path);
                file.close();
                return false;
            }

            const RawVcamRecord& raw = chunkBuffer[i];
            CameraRecord& out = outRecords[writeIndex++];
            out.latitudeDeg = raw.latitudeDeg;
            out.longitudeDeg = raw.longitudeDeg;
            out.snapLatitudeDeg = raw.snapLatitudeDeg;
            out.snapLongitudeDeg = raw.snapLongitudeDeg;
            out.bearingTenthsDeg = raw.bearingTenthsDeg;
            out.widthM = raw.widthM;
            out.toleranceDeg = raw.toleranceDeg;
            out.type = raw.type;
            out.speedLimit = raw.speedLimit;
            out.flags = raw.flags;
            out.reserved = raw.reserved;
            const bool hasSnapPoint = std::isfinite(raw.snapLatitudeDeg) && std::isfinite(raw.snapLongitudeDeg);
            const float cellLatitudeDeg = hasSnapPoint ? raw.snapLatitudeDeg : raw.latitudeDeg;
            const float cellLongitudeDeg = hasSnapPoint ? raw.snapLongitudeDeg : raw.longitudeDeg;
            out.cellKey = CameraIndex::encodeCellKey(cellLatitudeDeg, cellLongitudeDeg);
        }

        remaining -= toRead;
        if (remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    file.close();
    return true;
}

bool CameraDataLoader::buildSpans(CameraRecord* records,
                                  uint32_t recordCount,
                                  CameraCellSpan*& outSpans,
                                  uint32_t& outSpanCount) {
    outSpans = nullptr;
    outSpanCount = 0;
    if (!records || recordCount == 0) {
        return false;
    }

    uint32_t spanCount = 1;
    for (uint32_t i = 1; i < recordCount; ++i) {
        if (records[i].cellKey != records[i - 1].cellKey) {
            spanCount++;
        }
    }

    const uint32_t spanBytes = spanCount * sizeof(CameraCellSpan);
    if (spanBytes > CameraIndex::kSpanBudgetBytes) {
        Serial.printf("[CameraLoader] buildSpans failed: span budget exceeded (%lu > %lu)\n",
                      static_cast<unsigned long>(spanBytes),
                      static_cast<unsigned long>(CameraIndex::kSpanBudgetBytes));
        return false;
    }

    // Allocate spans in PSRAM to avoid internal SRAM contention with WiFi.
    // Binary search over spans is O(log n) — PSRAM latency is acceptable.
    CameraCellSpan* spans = static_cast<CameraCellSpan*>(
        heap_caps_malloc(spanBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!spans) {
        Serial.printf("[CameraLoader] buildSpans failed: PSRAM alloc returned null (%lu bytes)\n",
                      static_cast<unsigned long>(spanBytes));
        return false;
    }

    uint32_t spanIndex = 0;
    uint32_t beginIndex = 0;
    uint32_t currentCellKey = records[0].cellKey;
    for (uint32_t i = 1; i <= recordCount; ++i) {
        const bool boundary = (i == recordCount) || (records[i].cellKey != currentCellKey);
        if (!boundary) {
            continue;
        }

        spans[spanIndex].cellKey = currentCellKey;
        spans[spanIndex].beginIndex = beginIndex;
        spans[spanIndex].endIndex = i;
        spanIndex++;

        beginIndex = i;
        if (i < recordCount) {
            currentCellKey = records[i].cellKey;
        }

        if ((spanIndex & 0x03FFu) == 0u) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    outSpans = spans;
    outSpanCount = spanIndex;
    return spanIndex == spanCount;
}

void CameraDataLoader::publishReadyBuffers(CameraIndexOwnedBuffers& built) {
    CameraIndexOwnedBuffers stale{};
    portENTER_CRITICAL(&readyMux_);
    stale = readyBuffers_;
    readyBuffers_ = built;
    built = {};
    portEXIT_CRITICAL(&readyMux_);

    CameraIndex::freeOwnedBuffers(stale);
}

void CameraDataLoader::clearReadyBuffers() {
    CameraIndexOwnedBuffers stale{};
    portENTER_CRITICAL(&readyMux_);
    stale = readyBuffers_;
    readyBuffers_ = {};
    portEXIT_CRITICAL(&readyMux_);

    CameraIndex::freeOwnedBuffers(stale);
}
