#pragma once

#include <stdint.h>

struct LoopIngestContext {
    uint32_t nowMs = 0;
    bool bleProcessEnabled = false;
    void (*runBleProcess)() = nullptr;
    void (*runBleDrain)() = nullptr;
    bool skipNonCoreThisLoop = false;
    bool overloadThisLoop = false;
    bool obdServiceEnabled = false;
};

struct LoopIngestResult {
    bool bleBackpressure = false;
    bool skipLateNonCoreThisLoop = false;
    bool overloadLateThisLoop = false;
};

// Orchestrates BLE ingest, OBD runtime update, GPS update, and backpressure merge.
class LoopIngestModule {
public:
    struct Providers {
        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;

        void (*runBleProcess)(void* ctx) = nullptr;
        void* bleProcessContext = nullptr;
        void (*recordBleProcessUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* bleProcessPerfContext = nullptr;

        void (*runBleDrain)(void* ctx) = nullptr;
        void* bleDrainContext = nullptr;
        void (*recordBleDrainUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* bleDrainPerfContext = nullptr;
        bool (*readBleBackpressure)(void* ctx) = nullptr;
        void* bleBackpressureContext = nullptr;

        void (*runObdRuntime)(void* ctx, uint32_t nowMs, bool obdServiceEnabled) = nullptr;
        void* obdRuntimeContext = nullptr;
        void (*recordObdUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* obdPerfContext = nullptr;

        void (*runGpsRuntimeUpdate)(void* ctx, uint32_t nowMs) = nullptr;
        void* gpsRuntimeContext = nullptr;
        void (*recordGpsUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* gpsPerfContext = nullptr;
    };

    void begin(const Providers& hooks);
    void reset();
    LoopIngestResult process(const LoopIngestContext& ctx);

private:
    Providers providers{};
};
