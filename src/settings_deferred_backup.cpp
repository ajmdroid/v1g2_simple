#include "settings_internals.h"
#include "psram_freertos_alloc.h"

#include <atomic>
#include <esp_heap_caps.h>

namespace {

constexpr UBaseType_t SETTINGS_DEFERRED_BACKUP_QUEUE_DEPTH = 1;
constexpr uint32_t SETTINGS_DEFERRED_BACKUP_WRITER_STACK_SIZE = 6144;
constexpr UBaseType_t SETTINGS_DEFERRED_BACKUP_WRITER_PRIORITY = 1;
constexpr uint32_t SETTINGS_DEFERRED_BACKUP_RETRY_BACKOFF_MS = 250;

struct DeferredSettingsBackupState {
    QueueHandle_t queue = nullptr;
    TaskHandle_t writerTask = nullptr;
    PsramQueueAllocation queueAllocation = {};
    bool queueInPsram = false;
    bool writerTaskStackInPsram = false;
    std::atomic<bool> pendingRequest{false};
    std::atomic<bool> writerRetryPending{false};
    uint32_t nextAttemptAtMs = 0;
};

DeferredSettingsBackupState gDeferredSettingsBackupState;

bool isDeferredBackupRetryDue(uint32_t nowMs, uint32_t targetMs) {
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

void scheduleDeferredBackupRetry(uint32_t nowMs) {
    gDeferredSettingsBackupState.pendingRequest.store(true, std::memory_order_relaxed);
    gDeferredSettingsBackupState.nextAttemptAtMs = nowMs + SETTINGS_DEFERRED_BACKUP_RETRY_BACKOFF_MS;
}

void clearDeferredBackupRetry() {
    gDeferredSettingsBackupState.nextAttemptAtMs = 0;
}

bool writeDeferredBackupPayloadNow(const SerializedSettingsBackupPayload& payload) {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return false;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
    if (!lock) {
        return false;
    }

    return writeBackupAtomically(fs, payload);
}

bool processDeferredBackupQueueItem(SerializedSettingsBackupPayload& payload) {
    const bool ok = writeDeferredBackupPayloadNow(payload);
    if (!ok) {
        gDeferredSettingsBackupState.writerRetryPending.store(true, std::memory_order_relaxed);
    }
    releaseSerializedSettingsBackupPayload(payload);
    return ok;
}

void deferredBackupWriterTaskEntry(void*) {
    while (true) {
        SerializedSettingsBackupPayload payload;
        if (xQueueReceive(gDeferredSettingsBackupState.queue,
                          &payload,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        processDeferredBackupQueueItem(payload);
        taskYIELD();
    }
}

bool ensureDeferredBackupWriterReady() {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        gDeferredSettingsBackupState.queue = createQueuePreferPsram(
            SETTINGS_DEFERRED_BACKUP_QUEUE_DEPTH,
            sizeof(SerializedSettingsBackupPayload),
            gDeferredSettingsBackupState.queueAllocation,
            &gDeferredSettingsBackupState.queueInPsram);
        if (gDeferredSettingsBackupState.queue == nullptr) {
            Serial.println("[Settings] ERROR: Failed to create deferred backup queue");
            return false;
        }
    }

    if (gDeferredSettingsBackupState.writerTask == nullptr) {
        const BaseType_t rc = createTaskPinnedToCorePreferPsram(
            deferredBackupWriterTaskEntry,
            "SettingsBackup",
            SETTINGS_DEFERRED_BACKUP_WRITER_STACK_SIZE,
            nullptr,
            SETTINGS_DEFERRED_BACKUP_WRITER_PRIORITY,
            &gDeferredSettingsBackupState.writerTask,
            0,
            &gDeferredSettingsBackupState.writerTaskStackInPsram);
        if (rc != pdPASS) {
            Serial.println("[Settings] ERROR: Failed to create deferred backup writer task");
            return false;
        }
    }

    return true;
}

bool enqueueDeferredBackupPayload(SerializedSettingsBackupPayload& payload) {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        return false;
    }

    while (xQueueSend(gDeferredSettingsBackupState.queue, &payload, 0) != pdTRUE) {
        SerializedSettingsBackupPayload displaced;
        if (xQueueReceive(gDeferredSettingsBackupState.queue, &displaced, 0) != pdTRUE) {
            return false;
        }
        releaseSerializedSettingsBackupPayload(displaced);
    }

    return true;
}

}  // namespace

#ifdef UNIT_TEST
void resetDeferredSettingsBackupStateForTest() {
    if (gDeferredSettingsBackupState.queue != nullptr) {
        SerializedSettingsBackupPayload payload;
        while (xQueueReceive(gDeferredSettingsBackupState.queue, &payload, 0) == pdTRUE) {
            releaseSerializedSettingsBackupPayload(payload);
        }
        vQueueDelete(gDeferredSettingsBackupState.queue);
    }
    gDeferredSettingsBackupState.queue = nullptr;
    gDeferredSettingsBackupState.writerTask = nullptr;
    if (gDeferredSettingsBackupState.queueAllocation.queueBuffer != nullptr) {
        heap_caps_free(gDeferredSettingsBackupState.queueAllocation.queueBuffer);
        gDeferredSettingsBackupState.queueAllocation.queueBuffer = nullptr;
    }
    gDeferredSettingsBackupState.queueInPsram = false;
    gDeferredSettingsBackupState.writerTaskStackInPsram = false;
    gDeferredSettingsBackupState.pendingRequest.store(false, std::memory_order_relaxed);
    gDeferredSettingsBackupState.writerRetryPending.store(false, std::memory_order_relaxed);
    gDeferredSettingsBackupState.nextAttemptAtMs = 0;
}

bool runDeferredSettingsBackupWriterOnceForTest() {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        return false;
    }

    SerializedSettingsBackupPayload payload;
    if (xQueueReceive(gDeferredSettingsBackupState.queue, &payload, 0) != pdTRUE) {
        return false;
    }
    return processDeferredBackupQueueItem(payload);
}

size_t deferredSettingsBackupQueueDepthForTest() {
    if (gDeferredSettingsBackupState.queue == nullptr) {
        return 0;
    }
    return static_cast<size_t>(uxQueueMessagesWaiting(gDeferredSettingsBackupState.queue));
}

bool deferredSettingsBackupPendingForTest() {
    return gDeferredSettingsBackupState.pendingRequest.load(std::memory_order_relaxed) ||
           gDeferredSettingsBackupState.writerRetryPending.load(std::memory_order_relaxed);
}
#endif

void SettingsManager::saveDeferredBackup() {
    if (!persistSettingsAtomically()) {
        return;
    }

    clearDeferredPersistState();
    bumpBackupRevision();
    Serial.println("Settings saved atomically");
    requestDeferredBackupFromCurrentState();
}

void SettingsManager::requestDeferredBackupFromCurrentState() {
    gDeferredSettingsBackupState.pendingRequest.store(true, std::memory_order_relaxed);
    clearDeferredBackupRetry();
}

bool SettingsManager::deferredBackupPending() const {
    return gDeferredSettingsBackupState.pendingRequest.load(std::memory_order_relaxed) ||
           gDeferredSettingsBackupState.writerRetryPending.load(std::memory_order_relaxed);
}

bool SettingsManager::deferredBackupRetryScheduled() const {
    return gDeferredSettingsBackupState.nextAttemptAtMs != 0;
}

uint32_t SettingsManager::deferredBackupNextAttemptAtMs() const {
    return gDeferredSettingsBackupState.nextAttemptAtMs;
}

void SettingsManager::serviceDeferredBackup(uint32_t nowMs) {
    if (gDeferredSettingsBackupState.writerRetryPending.exchange(false, std::memory_order_relaxed)) {
        gDeferredSettingsBackupState.pendingRequest.store(true, std::memory_order_relaxed);
        clearDeferredBackupRetry();
    }

    if (!gDeferredSettingsBackupState.pendingRequest.load(std::memory_order_relaxed)) {
        return;
    }

    const uint32_t retryAtMs = gDeferredSettingsBackupState.nextAttemptAtMs;
    if (retryAtMs != 0 && !isDeferredBackupRetryDue(nowMs, retryAtMs)) {
        return;
    }

    if (!ensureDeferredBackupWriterReady()) {
        scheduleDeferredBackupRetry(nowMs);
        return;
    }

    {
        StorageManager::SDTryLock sdLock(storageManager.getSDMutex());
        if (!sdLock) {
            scheduleDeferredBackupRetry(nowMs);
            return;
        }
    }

    SerializedSettingsBackupPayload payload;
    if (!buildSerializedSdBackupPayload(payload, settings_, v1ProfileManager, nowMs)) {
        scheduleDeferredBackupRetry(nowMs);
        return;
    }

    if (!enqueueDeferredBackupPayload(payload)) {
        releaseSerializedSettingsBackupPayload(payload);
        scheduleDeferredBackupRetry(nowMs);
        return;
    }

    gDeferredSettingsBackupState.pendingRequest.store(false, std::memory_order_relaxed);
    clearDeferredBackupRetry();
}
