#include "camera_data_loader.h"

#include "../../storage_manager.h"

#include <Arduino.h>
#include <FS.h>
#include <algorithm>
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
    portEXIT_CRITICAL(&statusMux_);
}

void CameraDataLoader::requestReload() {
    reloadPending_.store(true, std::memory_order_relaxed);

    portENTER_CRITICAL(&statusMux_);
    status_.reloadPending = true;
    portEXIT_CRITICAL(&statusMux_);

    if (loaderTask_) {
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
    return out;
}

void CameraDataLoader::loaderTaskEntry(void* param) {
    CameraDataLoader* self = static_cast<CameraDataLoader*>(param);
    self->loaderTaskLoop();
}

void CameraDataLoader::loaderTaskLoop() {
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!reloadPending_.exchange(false, std::memory_order_relaxed)) {
            continue;
        }

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
        const bool ok = buildEnforcementIndex(built);
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
            status_.loadFailures++;
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
}

bool CameraDataLoader::buildEnforcementIndex(CameraIndexOwnedBuffers& outBuffers) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    uint32_t totalRecords = 0;
    if (!accumulateRecordCount(totalRecords) || totalRecords == 0) {
        return false;
    }

    const uint32_t requiredBytes = totalRecords * sizeof(CameraRecord);
    const uint32_t largestPsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (largestPsram < (requiredBytes + kPsramHeadroomBytes)) {
        return false;
    }

    CameraRecord* records = static_cast<CameraRecord*>(
        heap_caps_malloc(requiredBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!records) {
        return false;
    }

    bool ok = loadRecords(records, totalRecords);
    if (!ok) {
        heap_caps_free(records);
        return false;
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
        heap_caps_free(records);
        return false;
    }

    outBuffers.records = records;
    outBuffers.recordCount = totalRecords;
    outBuffers.spans = spans;
    outBuffers.spanCount = spanCount;
    outBuffers.version = nextVersion_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool CameraDataLoader::accumulateRecordCount(uint32_t& outTotalRecords) {
    outTotalRecords = 0;
    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        return false;
    }

    for (const char* path : kDatasetFiles) {
        File file = fs->open(path, FILE_READ);
        if (!file) {
            return false;
        }

        VcamHeader header{};
        const size_t headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
        const bool validHeader =
            std::memcmp(header.magic, "VCAM", 4) == 0 &&
            header.version == kExpectedVersion &&
            header.recordSize == kExpectedRecordSize;
        if (headerRead != sizeof(header) || !validHeader) {
            file.close();
            return false;
        }

        const size_t expectedBytes = sizeof(VcamHeader) + (header.count * header.recordSize);
        if (file.size() < expectedBytes) {
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

    RawVcamRecord* chunkBuffer = static_cast<RawVcamRecord*>(
        heap_caps_malloc(sizeof(RawVcamRecord) * kChunkRecords, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!chunkBuffer) {
        chunkBuffer = static_cast<RawVcamRecord*>(
            heap_caps_malloc(sizeof(RawVcamRecord) * kChunkRecords, MALLOC_CAP_8BIT));
    }
    if (!chunkBuffer) {
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
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        return false;
    }

    File file = fs->open(path, FILE_READ);
    if (!file) {
        return false;
    }

    VcamHeader header{};
    const size_t headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    const bool validHeader =
        std::memcmp(header.magic, "VCAM", 4) == 0 &&
        header.version == kExpectedVersion &&
        header.recordSize == kExpectedRecordSize;
    if (headerRead != sizeof(header) || !validHeader) {
        file.close();
        return false;
    }

    uint32_t remaining = header.count;
    while (remaining > 0) {
        const uint32_t toRead = std::min<uint32_t>(chunkCapacity, remaining);
        const size_t bytesToRead = static_cast<size_t>(toRead) * sizeof(RawVcamRecord);
        const size_t bytesRead = file.read(reinterpret_cast<uint8_t*>(chunkBuffer), bytesToRead);
        if (bytesRead != bytesToRead) {
            file.close();
            return false;
        }

        for (uint32_t i = 0; i < toRead; ++i) {
            if (writeIndex >= totalRecords) {
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
            out.cellKey = CameraIndex::encodeCellKey(raw.latitudeDeg, raw.longitudeDeg);
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
    if (spanBytes > CameraIndex::kSpanSramBudgetBytes) {
        return false;
    }

    CameraCellSpan* spans = static_cast<CameraCellSpan*>(
        heap_caps_malloc(spanBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!spans) {
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
