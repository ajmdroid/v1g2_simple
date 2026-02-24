#include "connection_state_cadence_module.h"

void ConnectionStateCadenceModule::reset() {
    lastDisplayUpdateMs = 0;
    scanScreenEnteredMs = 0;
    scanScreenDwellActive = false;
    lastBleConnectedForScanDwell = false;
}

void ConnectionStateCadenceModule::onScanningScreenShown(unsigned long nowMs) {
    scanScreenEnteredMs = nowMs;
    scanScreenDwellActive = true;
}

ConnectionStateCadenceDecision ConnectionStateCadenceModule::process(
        const ConnectionStateCadenceContext& ctx) {
    ConnectionStateCadenceDecision decision;

    if (lastBleConnectedForScanDwell && !ctx.bleConnectedNow && !ctx.bootSplashHoldActive) {
        scanScreenEnteredMs = ctx.nowMs;
        scanScreenDwellActive = true;
    }
    lastBleConnectedForScanDwell = ctx.bleConnectedNow;

    if ((ctx.nowMs - lastDisplayUpdateMs) < ctx.displayUpdateIntervalMs) {
        return decision;
    }
    lastDisplayUpdateMs = ctx.nowMs;
    decision.displayUpdateDue = true;

    if (ctx.displayPreviewRunning || ctx.bootSplashHoldActive) {
        return decision;
    }

    if (scanScreenDwellActive && ctx.bleConnectedNow) {
        const unsigned long scanDwellMs = ctx.nowMs - scanScreenEnteredMs;
        decision.holdScanDwell = scanDwellMs < ctx.scanScreenDwellMs;
    }

    if (!decision.holdScanDwell) {
        decision.shouldRunConnectionStateProcess = true;
        if (ctx.bleConnectedNow) {
            scanScreenDwellActive = false;
        }
    }

    return decision;
}
