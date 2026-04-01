#include "periodic_maintenance_module.h"
#include <esp_heap_caps.h>
#include <Arduino.h>

#if PERF_METRICS
static uint32_t sLastHeapIntegrityCheckMs = 0;
static constexpr uint32_t HEAP_INTEGRITY_CHECK_INTERVAL_MS = 30000;  // Every 30 seconds
#endif

void PeriodicMaintenanceModule::begin(const Providers& hooks) {
    providers = hooks;
}

void PeriodicMaintenanceModule::process(uint32_t nowMs) {
#if PERF_METRICS
    // Periodic heap integrity check (every ~30 seconds)
    if (sLastHeapIntegrityCheckMs == 0 ||
        (nowMs - sLastHeapIntegrityCheckMs >= HEAP_INTEGRITY_CHECK_INTERVAL_MS)) {
        sLastHeapIntegrityCheckMs = nowMs;

        if (!heap_caps_check_integrity_all(true)) {
            Serial.println("[HEAP] !!! INTEGRITY CHECK FAILED !!!");
        }
    }
#endif
    if (providers.runPerfReport) {
        uint32_t startUs = 0;
        if (providers.timestampUs) {
            startUs = providers.timestampUs(providers.timestampContext);
        }

        providers.runPerfReport(providers.perfReportContext);

        if (providers.recordPerfReportUs && providers.timestampUs) {
            const uint32_t elapsedUs =
                static_cast<uint32_t>(providers.timestampUs(providers.timestampContext) - startUs);
            providers.recordPerfReportUs(providers.perfReportRecordContext, elapsedUs);
        }
    }

    if (providers.runTimeSave) {
        uint32_t startUs = 0;
        if (providers.timestampUs) {
            startUs = providers.timestampUs(providers.timestampContext);
        }

        providers.runTimeSave(providers.timeSaveContext, nowMs);

        if (providers.recordTimeSaveUs && providers.timestampUs) {
            const uint32_t elapsedUs =
                static_cast<uint32_t>(providers.timestampUs(providers.timestampContext) - startUs);
            providers.recordTimeSaveUs(providers.timeSaveRecordContext, elapsedUs);
        }
    }

    if (providers.runObdSettingsSync) {
        providers.runObdSettingsSync(providers.obdSettingsSyncContext, nowMs);
    }

    if (providers.runDeferredSettingsPersist) {
        providers.runDeferredSettingsPersist(providers.deferredSettingsPersistContext, nowMs);
    }

    if (providers.runDeferredSettingsBackup) {
        providers.runDeferredSettingsBackup(providers.deferredSettingsBackupContext, nowMs);
    }

    if (providers.runDeferredBleBondBackup) {
        providers.runDeferredBleBondBackup(providers.deferredBleBondBackupContext, nowMs);
    }

    if (providers.runStoreSave) {
        providers.runStoreSave(providers.storeSaveContext, nowMs);
    }
}
