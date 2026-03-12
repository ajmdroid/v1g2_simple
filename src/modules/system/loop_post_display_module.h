#pragma once

#include <stdint.h>

#include "modules/ble/connection_state_dispatch_module.h"

struct LoopPostDisplayContext {
    bool enableAutoPush = true;
    bool runSpeedAndDispatch = true;

    uint32_t nowMs = 0;
    bool skipLateNonCoreThisLoop = false;
    bool overloadLateThisLoop = false;
    bool loopSignalPriorityActive = false;

    uint32_t displayUpdateIntervalMs = 50;
    uint32_t scanScreenDwellMs = 0;
    bool bootSplashHoldActive = false;
    bool displayPreviewRunning = false;
    uint32_t maxProcessGapMs = 0;
    bool bleConnectedNow = false;

    void (*runAutoPush)() = nullptr;
    void (*runConnectionStateDispatch)(const ConnectionStateDispatchContext& dispatchCtx) = nullptr;
};

struct LoopPostDisplayResult {
    uint32_t dispatchNowMs = 0;
    bool bleConnectedNow = false;
};

// Orchestrates post-display runtime work: auto-push,
// runtime, and connection-state dispatch cadence/watchdog.
class LoopPostDisplayModule {
public:
    struct Providers {
        void (*runAutoPush)(void* ctx) = nullptr;
        void* autoPushContext = nullptr;

        uint32_t (*timestampUs)(void* ctx) = nullptr;
        void* timestampContext = nullptr;
        uint32_t (*readDispatchNowMs)(void* ctx) = nullptr;
        void* dispatchNowContext = nullptr;
        bool (*readBleConnectedNow)(void* ctx) = nullptr;
        void* bleConnectedContext = nullptr;

        void (*runConnectionStateDispatch)(void* ctx, const ConnectionStateDispatchContext& dispatchCtx) = nullptr;
        void* connectionDispatchContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopPostDisplayResult process(const LoopPostDisplayContext& ctx);

private:
    Providers providers{};
};
