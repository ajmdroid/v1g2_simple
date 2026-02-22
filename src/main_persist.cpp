/**
 * main_persist.cpp — Periodic persistence helpers extracted from loop().
 *
 * These are self-contained save state machines for lockout zones and
 * learner candidates.  They use rate-limiting, dirty flags, and
 * non-blocking SD try-locks (Tier 7 — best-effort, never block).
 */

#include "main_internals.h"
#include "perf_metrics.h"
#include "storage_manager.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/lockout_learner.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

namespace {
// Keep background SD writes out of the danger zone during AP+STA operation.
// These thresholds match the WiFi low-memory guard that has proven stable.
static constexpr uint32_t LOCKOUT_SAVE_MIN_DMA_FREE = 20480;
static constexpr uint32_t LOCKOUT_SAVE_MIN_DMA_BLOCK = 8192;
// If dirty data remains unsaved for too long, allow a cautious retry using a
// slightly lower free-heap floor but stricter largest-block requirement.
static constexpr uint32_t LOCKOUT_SAVE_AGED_DMA_FREE = 17600;
static constexpr uint32_t LOCKOUT_SAVE_AGED_DMA_BLOCK = 12288;
static constexpr uint32_t LOCKOUT_SAVE_MAX_DIRTY_AGE_MS = 300000;  // 5 minutes

inline bool hasDmaHeadroomForBackgroundSave(uint32_t& freeDma, uint32_t& largestDma) {
    freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    largestDma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    return (freeDma >= LOCKOUT_SAVE_MIN_DMA_FREE) && (largestDma >= LOCKOUT_SAVE_MIN_DMA_BLOCK);
}

inline bool hasAgedDmaHeadroomForBackgroundSave(uint32_t freeDma, uint32_t largestDma) {
    return (freeDma >= LOCKOUT_SAVE_AGED_DMA_FREE) && (largestDma >= LOCKOUT_SAVE_AGED_DMA_BLOCK);
}
}  // namespace

// ---- processLockoutStoreSave ----

void processLockoutStoreSave(uint32_t nowMs) {
    uint32_t lockoutSaveStartUs = PERF_TIMESTAMP_US();
    static uint32_t lastLockoutSaveMs = 0;
    static uint32_t lastLockoutSaveAttemptMs = 0;
    static uint32_t lockoutDirtySinceMs = 0;
    static constexpr uint32_t LOCKOUT_SAVE_INTERVAL_MS = 60000;  // 60s
    static constexpr uint32_t LOCKOUT_SAVE_RETRY_MS = 15000;     // Retry backoff on contention/failure
    if (lockoutStore.isDirty()) {
        if (lockoutDirtySinceMs == 0) {
            lockoutDirtySinceMs = nowMs;
        }
    } else {
        lockoutDirtySinceMs = 0;
    }
    if (lockoutStore.isDirty() && storageManager.isReady() &&
        (nowMs - lastLockoutSaveMs) >= LOCKOUT_SAVE_INTERVAL_MS &&
        (nowMs - lastLockoutSaveAttemptMs) >= LOCKOUT_SAVE_RETRY_MS) {
        lastLockoutSaveAttemptMs = nowMs;
        static constexpr const char* LOCKOUT_ZONES_PATH = "/v1simple_lockout_zones.json";
        fs::FS* fs = storageManager.getFilesystem();
        bool saveOk = false;
        bool saveDeferred = false;
        if (fs) {
            if (storageManager.isSDCard()) {
                // Core 1 path must never block waiting for SD ownership.
                uint32_t freeDma = 0;
                uint32_t largestDma = 0;
                const bool normalHeadroom = hasDmaHeadroomForBackgroundSave(freeDma, largestDma);
                const uint32_t dirtyAgeMs = (lockoutDirtySinceMs == 0) ? 0 : (nowMs - lockoutDirtySinceMs);
                const bool allowAgedRetry =
                    !normalHeadroom &&
                    (dirtyAgeMs >= LOCKOUT_SAVE_MAX_DIRTY_AGE_MS) &&
                    hasAgedDmaHeadroomForBackgroundSave(freeDma, largestDma);

                if (normalHeadroom || allowAgedRetry) {
                    if (allowAgedRetry) {
                        static uint32_t lastAgedRetryLogMs = 0;
                        if ((nowMs - lastAgedRetryLogMs) >= 10000) {
                            lastAgedRetryLogMs = nowMs;
                            Serial.printf("[Lockout] Save retry (aged dirty=%lus free=%lu block=%lu relaxed>=%lu/%lu)\n",
                                          static_cast<unsigned long>(dirtyAgeMs / 1000),
                                          static_cast<unsigned long>(freeDma),
                                          static_cast<unsigned long>(largestDma),
                                          static_cast<unsigned long>(LOCKOUT_SAVE_AGED_DMA_FREE),
                                          static_cast<unsigned long>(LOCKOUT_SAVE_AGED_DMA_BLOCK));
                        }
                    }
                    StorageManager::SDTryLock sdLock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
                    if (sdLock) {
                        JsonDocument doc;
                        lockoutStore.toJson(doc);
                        saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_ZONES_PATH, doc);
                    } else {
                        saveDeferred = true;
                        static uint32_t lastLockoutSaveSkipLogMs = 0;
                        if ((nowMs - lastLockoutSaveSkipLogMs) >= 10000) {
                            lastLockoutSaveSkipLogMs = nowMs;
                            Serial.println("[Lockout] Save deferred (SD busy)");
                        }
                    }
                } else {
                    saveDeferred = true;
                    static uint32_t lastLowDmaLogMs = 0;
                    if ((nowMs - lastLowDmaLogMs) >= 10000) {
                        lastLowDmaLogMs = nowMs;
                        Serial.printf("[Lockout] Save deferred (low DMA heap free=%lu block=%lu need>=%lu/%lu dirty=%lus)\n",
                                      static_cast<unsigned long>(freeDma),
                                      static_cast<unsigned long>(largestDma),
                                      static_cast<unsigned long>(LOCKOUT_SAVE_MIN_DMA_FREE),
                                      static_cast<unsigned long>(LOCKOUT_SAVE_MIN_DMA_BLOCK),
                                      static_cast<unsigned long>(dirtyAgeMs / 1000));
                    }
                }
            } else {
                // LittleFS fallback path (single-CPU filesystem access).
                JsonDocument doc;
                lockoutStore.toJson(doc);
                saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_ZONES_PATH, doc);
            }
        }
        if (saveOk) {
            lastLockoutSaveMs = nowMs;
            lockoutStore.clearDirty();
            lockoutDirtySinceMs = 0;
            Serial.printf("[Lockout] Saved %lu zones to %s\n",
                          static_cast<unsigned long>(lockoutStore.stats().entriesSaved),
                          LOCKOUT_ZONES_PATH);
        } else if (!saveDeferred) {
            Serial.println("[Lockout] Save failed");
        }
    }
    perfRecordLockoutSaveUs(PERF_TIMESTAMP_US() - lockoutSaveStartUs);
}

// ---- processLearnerPendingSave ----

void processLearnerPendingSave(uint32_t nowMs) {
    uint32_t learnerSaveStartUs = PERF_TIMESTAMP_US();
    static uint32_t lastLearnerSaveMs = 0;
    static uint32_t lastLearnerSaveAttemptMs = 0;
    static uint32_t learnerDirtySinceMs = 0;
    static constexpr uint32_t LEARNER_SAVE_INTERVAL_MS = 15000;  // 15s
    static constexpr uint32_t LEARNER_SAVE_RETRY_MS = 15000;     // Retry backoff
    if (lockoutLearner.isDirty()) {
        if (learnerDirtySinceMs == 0) {
            learnerDirtySinceMs = nowMs;
        }
    } else {
        learnerDirtySinceMs = 0;
    }
    if (lockoutLearner.isDirty() && storageManager.isReady() &&
        (nowMs - lastLearnerSaveMs) >= LEARNER_SAVE_INTERVAL_MS &&
        (nowMs - lastLearnerSaveAttemptMs) >= LEARNER_SAVE_RETRY_MS) {
        lastLearnerSaveAttemptMs = nowMs;
        static constexpr const char* LOCKOUT_PENDING_PATH = "/v1simple_lockout_pending.json";
        fs::FS* fs = storageManager.getFilesystem();
        bool saveOk = false;
        bool saveDeferred = false;
        if (fs) {
            if (storageManager.isSDCard()) {
                // Core 1 path must never block waiting for SD ownership.
                uint32_t freeDma = 0;
                uint32_t largestDma = 0;
                const bool normalHeadroom = hasDmaHeadroomForBackgroundSave(freeDma, largestDma);
                const uint32_t dirtyAgeMs = (learnerDirtySinceMs == 0) ? 0 : (nowMs - learnerDirtySinceMs);
                const bool allowAgedRetry =
                    !normalHeadroom &&
                    (dirtyAgeMs >= LOCKOUT_SAVE_MAX_DIRTY_AGE_MS) &&
                    hasAgedDmaHeadroomForBackgroundSave(freeDma, largestDma);

                if (normalHeadroom || allowAgedRetry) {
                    if (allowAgedRetry) {
                        static uint32_t lastAgedRetryLogMs = 0;
                        if ((nowMs - lastAgedRetryLogMs) >= 10000) {
                            lastAgedRetryLogMs = nowMs;
                            Serial.printf("[Learner] Save retry (aged dirty=%lus free=%lu block=%lu relaxed>=%lu/%lu)\n",
                                          static_cast<unsigned long>(dirtyAgeMs / 1000),
                                          static_cast<unsigned long>(freeDma),
                                          static_cast<unsigned long>(largestDma),
                                          static_cast<unsigned long>(LOCKOUT_SAVE_AGED_DMA_FREE),
                                          static_cast<unsigned long>(LOCKOUT_SAVE_AGED_DMA_BLOCK));
                        }
                    }
                    StorageManager::SDTryLock sdLock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
                    if (sdLock) {
                        JsonDocument doc;
                        lockoutLearner.toJson(doc);
                        saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_PENDING_PATH, doc);
                    } else {
                        saveDeferred = true;
                    }
                } else {
                    saveDeferred = true;
                    static uint32_t lastLowDmaLogMs = 0;
                    if ((nowMs - lastLowDmaLogMs) >= 10000) {
                        lastLowDmaLogMs = nowMs;
                        Serial.printf("[Learner] Save deferred (low DMA heap free=%lu block=%lu need>=%lu/%lu dirty=%lus)\n",
                                      static_cast<unsigned long>(freeDma),
                                      static_cast<unsigned long>(largestDma),
                                      static_cast<unsigned long>(LOCKOUT_SAVE_MIN_DMA_FREE),
                                      static_cast<unsigned long>(LOCKOUT_SAVE_MIN_DMA_BLOCK),
                                      static_cast<unsigned long>(dirtyAgeMs / 1000));
                    }
                }
            } else {
                // LittleFS fallback path (single-CPU filesystem access).
                JsonDocument doc;
                lockoutLearner.toJson(doc);
                saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_PENDING_PATH, doc);
            }
        }
        if (saveOk) {
            lastLearnerSaveMs = nowMs;
            lockoutLearner.clearDirty();
            learnerDirtySinceMs = 0;
            Serial.printf("[Learner] Saved %u pending candidates to %s\n",
                          static_cast<unsigned>(lockoutLearner.activeCandidateCount()),
                          LOCKOUT_PENDING_PATH);
        } else if (!saveDeferred) {
            Serial.println("[Learner] Pending save failed");
        }
    }
    perfRecordLearnerSaveUs(PERF_TIMESTAMP_US() - learnerSaveStartUs);
}
