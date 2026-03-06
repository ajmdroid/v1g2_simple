#pragma once

#include <stdint.h>

#include "camera_alert_types.h"

class RoadMapReader;
class SettingsManager;
struct CameraResult;
struct V1Settings;

class CameraAlertModule {
public:
    CameraAlertModule() = default;

    void begin(RoadMapReader* roadMap, SettingsManager* settings);
    void process(uint32_t nowMs, const CameraAlertContext& ctx);

    bool isDisplayActive() const { return displayPayload_.active; }
    bool consumePendingVoice(CameraVoiceEvent& event);
    void onVoicePlaybackResult(const CameraVoiceEvent& event, bool playbackStarted);
    void resetEncounter();

    const CameraAlertDisplayPayload& displayPayload() const { return displayPayload_; }

private:
    enum class ApproachState : uint8_t {
        IDLE = 0,
        DETECTED,
        APPROACHING,
        CONFIRMED,
    };

    struct Breadcrumb {
        int32_t latE5 = 0;
        int32_t lonE5 = 0;
    };

    static constexpr uint8_t kBreadcrumbCapacity = 4;

    void clearBreadcrumbs();
    void clearEncounterState();
    void beginEncounter(const CameraResult& result);
    bool matchesEncounter(const CameraResult& result) const;
    void recordBreadcrumb(const CameraAlertContext& ctx);
    bool resolveTravelHeadingDeg(const CameraAlertContext& ctx, float& headingDeg) const;
    bool isTypeEnabled(const V1Settings& settings, CameraType type) const;
    void queueVoice(CameraType type, bool isNearStage);
    void maybeQueueVoice(const V1Settings& settings, CameraType type, uint32_t distanceCm);
    void expireEncounterIfNeeded(uint32_t nowMs);
    void deactivateDisplay();

    RoadMapReader* roadMap_ = nullptr;
    SettingsManager* settings_ = nullptr;

    Breadcrumb breadcrumbs_[kBreadcrumbCapacity]{};
    uint8_t breadcrumbCount_ = 0;
    uint8_t breadcrumbWriteIndex_ = 0;

    ApproachState state_ = ApproachState::IDLE;
    int32_t encounterLatE5_ = 0;
    int32_t encounterLonE5_ = 0;
    uint8_t encounterFlags_ = 0;
    uint32_t lastDistanceCm_ = UINT32_MAX;
    uint32_t lastSeenMs_ = 0;
    uint8_t closingPollCount_ = 0;
    bool farAnnounced_ = false;
    bool nearAnnounced_ = false;

    bool pendingVoiceValid_ = false;
    CameraVoiceEvent pendingVoice_{};

    CameraAlertDisplayPayload displayPayload_{};

    uint32_t lastPollMs_ = 0;
    bool hasPolled_ = false;
};

extern CameraAlertModule cameraAlertModule;
