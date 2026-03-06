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
    int onVoicePlaybackResultCalls = 0;
    bool lastPlaybackStarted = false;
    CameraVoiceEvent lastPlaybackEvent{};

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

    void onVoicePlaybackResult(const CameraVoiceEvent& event, bool playbackStarted) {
        ++onVoicePlaybackResultCalls;
        lastPlaybackEvent = event;
        lastPlaybackStarted = playbackStarted;
        if (!playbackStarted) {
            pendingVoiceValid = true;
            pendingVoice = event;
        }
    }

    void resetEncounter() {
        payload = CameraAlertDisplayPayload{};
        pendingVoiceValid = false;
        pendingVoice = CameraVoiceEvent{};
        onVoicePlaybackResultCalls = 0;
        lastPlaybackStarted = false;
        lastPlaybackEvent = CameraVoiceEvent{};
    }

    const CameraAlertDisplayPayload& displayPayload() const { return payload; }
};

inline CameraAlertModule cameraAlertModule;
