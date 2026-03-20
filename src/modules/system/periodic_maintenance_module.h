#pragma once

#include <stdint.h>

// Coordinates loop-tail periodic maintenance actions while preserving call order.
class PeriodicMaintenanceModule {
public:
    struct Providers {
        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;

        void (*runPerfReport)(void* ctx) = nullptr;
        void* perfReportContext = nullptr;
        void (*recordPerfReportUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* perfReportRecordContext = nullptr;

        void (*runTimeSave)(void* ctx, uint32_t nowMs) = nullptr;
        void* timeSaveContext = nullptr;
        void (*recordTimeSaveUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* timeSaveRecordContext = nullptr;

        void (*runObdSettingsSync)(void* ctx, uint32_t nowMs) = nullptr;
        void* obdSettingsSyncContext = nullptr;

        void (*runDeferredSettingsPersist)(void* ctx, uint32_t nowMs) = nullptr;
        void* deferredSettingsPersistContext = nullptr;

        void (*runDeferredSettingsBackup)(void* ctx, uint32_t nowMs) = nullptr;
        void* deferredSettingsBackupContext = nullptr;

        void (*runDeferredBleBondBackup)(void* ctx, uint32_t nowMs) = nullptr;
        void* deferredBleBondBackupContext = nullptr;

        int64_t (*nowEpochMsOr0)(void* ctx) = nullptr;
        void* epochContext = nullptr;
        void (*runLockoutLearner)(void* ctx, uint32_t nowMs, int64_t epochMs) = nullptr;
        void* lockoutLearnerContext = nullptr;

        void (*runLockoutStoreSave)(void* ctx, uint32_t nowMs) = nullptr;
        void* lockoutStoreSaveContext = nullptr;
        void (*runLearnerPendingSave)(void* ctx, uint32_t nowMs) = nullptr;
        void* learnerPendingSaveContext = nullptr;
    };

    void begin(const Providers& hooks);
    void process(uint32_t nowMs);

private:
    Providers providers{};
};
