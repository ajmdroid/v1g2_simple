#pragma once

#include <cstdint>

#include "../../../../include/camera_alert_types.h"

class RoadMapReader;
class SettingsManager;

class CameraAlertModule {
public:
    int processCalls = 0;
    uint32_t lastNowMs = 0;
    CameraAlertContext lastContext{};
    CameraAlertDisplayPayload payload{};

    void begin(RoadMapReader* /*roadMap*/, SettingsManager* /*settings*/) {}

    void process(uint32_t nowMs, const CameraAlertContext& ctx) {
        ++processCalls;
        lastNowMs = nowMs;
        lastContext = ctx;
    }

    bool isDisplayActive() const { return payload.active; }

    void resetEncounter() {
        payload = CameraAlertDisplayPayload{};
    }

    const CameraAlertDisplayPayload& displayPayload() const { return payload; }
};

inline CameraAlertModule cameraAlertModule;
