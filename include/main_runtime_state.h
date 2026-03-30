#pragma once
#ifndef MAIN_RUNTIME_STATE_H
#define MAIN_RUNTIME_STATE_H

struct MainRuntimeState {
    bool bootReady = false;
    unsigned long bootReadyDeadlineMs = 0;
    bool bootSplashHoldActive = false;
    unsigned long bootSplashHoldUntilMs = 0;
    bool initialScanningScreenShown = false;
    unsigned long activeScanScreenDwellMs = 0;
    unsigned long v1ConnectedAtMs = 0;
    bool wifiAutoStartDone = false;
    unsigned long lastLoopUs = 0;
};

#endif  // MAIN_RUNTIME_STATE_H
