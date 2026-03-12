#include "connection_runtime_module.h"

#include "modules/perf/debug_macros.h"

void ConnectionRuntimeModule::begin(const Providers& hooks) {
    begin(hooks, Config{});
}

void ConnectionRuntimeModule::begin(const Providers& hooks,
                                    const Config& cfg) {
    providers = hooks;
    config = cfg;
    reset();
}

void ConnectionRuntimeModule::reset() {
    lastAudioTickUs = 0;
    runStartLogged = false;
}

ConnectionRuntimeSnapshot ConnectionRuntimeModule::process(unsigned long nowMs,
                                                           unsigned long nowUs,
                                                           unsigned long lastLoopUs,
                                                           bool bootSplashHoldActive,
                                                           unsigned long bootSplashHoldUntilMs,
                                                           bool initialScanningScreenShown) {
    ConnectionRuntimeSnapshot snapshot;

    const bool connectedNow =
        providers.isBleConnected ? providers.isBleConnected(providers.bleContext) : false;

    snapshot.bootSplashHoldActive = bootSplashHoldActive;
    snapshot.initialScanningScreenShown = initialScanningScreenShown;

    if (snapshot.bootSplashHoldActive && static_cast<int32_t>(nowMs - bootSplashHoldUntilMs) >= 0) {
        snapshot.bootSplashHoldActive = false;
        if (!connectedNow) {
            snapshot.requestShowInitialScanning = true;
        } else {
            snapshot.initialScanningScreenShown = true;
        }
    }

    snapshot.connected = connectedNow;

    const unsigned long sinceAudioUs = nowUs - lastAudioTickUs;
    lastAudioTickUs = nowUs;

    snapshot.backpressured =
        providers.isBackpressured ? providers.isBackpressured(providers.queueContext) : false;
    snapshot.skipNonCore = (sinceAudioUs > config.audioTickMaxUs) || snapshot.backpressured;
    snapshot.overloaded = (lastLoopUs >= config.overloadLoopUs) || snapshot.skipNonCore;

    const unsigned long lastRxMs =
        providers.getLastRxMillis ? providers.getLastRxMillis(providers.queueContext) : 0;
    snapshot.receiving = (nowMs - lastRxMs) < config.receivingHeartbeatMs;

    if (!runStartLogged) {
        const bool bleReady = snapshot.connected;
        const bool timeReady = (nowMs >= config.runStartTimeoutMs);
        if (bleReady || timeReady) {
            runStartLogged = true;
            const char* trigger = bleReady ? "ble_connected" : "timeout_30s";
            SerialLog.printf("RUN_START trigger=%s millis=%lu\n", trigger, nowMs);
        }
    }

    return snapshot;
}
