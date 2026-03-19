#include "periodic_maintenance_module.h"

void PeriodicMaintenanceModule::begin(const Providers& hooks) {
    providers = hooks;
}

void PeriodicMaintenanceModule::process(uint32_t nowMs) {
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

    if (providers.runDeferredSettingsBackup) {
        providers.runDeferredSettingsBackup(providers.deferredSettingsBackupContext, nowMs);
    }

    if (providers.runDeferredBleBondBackup) {
        providers.runDeferredBleBondBackup(providers.deferredBleBondBackupContext, nowMs);
    }

    int64_t epochMs = 0;
    if (providers.nowEpochMsOr0) {
        epochMs = providers.nowEpochMsOr0(providers.epochContext);
    }

    if (providers.runLockoutLearner) {
        providers.runLockoutLearner(providers.lockoutLearnerContext, nowMs, epochMs);
    }

    if (providers.runLockoutStoreSave) {
        providers.runLockoutStoreSave(providers.lockoutStoreSaveContext, nowMs);
    }

    if (providers.runLearnerPendingSave) {
        providers.runLearnerPendingSave(providers.learnerPendingSaveContext, nowMs);
    }
}
