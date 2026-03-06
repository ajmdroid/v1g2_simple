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
    bool pendingVoiceValid = false;
    CameraVoiceEvent pendingVoice{};

    void begin(RoadMapReader* /*roadMap*/, SettingsManager* /*settings*/) {}

    void process(uint32_t nowMs, const CameraAlertContext& ctx) {
        ++processCalls;
        lastNowMs = nowMs;
        lastContext = ctx;
    }

    bool isDisplayActive() const { return payload.active; }

    bool consumePendingVoice(CameraVoiceEvent& event) {
        if (!pendingVoiceValid) {
            return false;
        }
        event = pendingVoice;
        pendingVoiceValid = false;
        pendingVoice = CameraVoiceEvent{};
        return true;
    }

    void resetEncounter() {
        payload = CameraAlertDisplayPayload{};
        pendingVoiceValid = false;
        pendingVoice = CameraVoiceEvent{};
    }

    const CameraAlertDisplayPayload& displayPayload() const { return payload; }
};

inline CameraAlertModule cameraAlertModule;
