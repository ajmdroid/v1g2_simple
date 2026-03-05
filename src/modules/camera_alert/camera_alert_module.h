#pragma once

#include <stdint.h>

#include "../../../include/camera_alert_types.h"
#include "modules/lockout/road_map_reader.h"
#include "../../settings.h"

struct GpsRuntimeStatus;

struct CameraAlertResult {
    bool displayActive = false;
    bool suppressedByV1 = false;
    CameraAlertDisplayPayload payload{};
};

struct CameraAlertStatusSnapshot {
    bool encounterActive = false;
    bool displayActive = false;
    bool pendingFar = false;
    bool pendingNear = false;
    bool farAnnounced = false;
    bool nearAnnounced = false;
    bool hasPayload = false;
    uint16_t distanceCm = 0xFFFF;
    uint8_t flags = 0;
    const char* typeName = "unknown";
};

struct CameraAlertContext {
    const V1Settings* settings = nullptr;
    const GpsRuntimeStatus* gpsStatus = nullptr;
    bool v1SignalPriorityActive = false;
    bool v1PersistedPriorityActive = false;
};

struct CameraAlertProviders {
    CameraResult (*nearestCamera)(void* context,
                                  int32_t latE5,
                                  int32_t lonE5,
                                  uint16_t searchRadiusE5) = nullptr;
    uint32_t (*cameraCount)(void* context) = nullptr;
    void* context = nullptr;
};

class CameraAlertModule {
public:
    static constexpr uint16_t CAMERA_FAR_THRESHOLD_CM = 30480;   // 1000 ft
    static constexpr uint16_t CAMERA_NEAR_THRESHOLD_CM = 15240;  // 500 ft
    static constexpr uint32_t CAMERA_POLL_INTERVAL_MS = 500;
    static constexpr uint32_t ENCOUNTER_EXPIRE_MS = 10000;
    static constexpr uint32_t CAMERA_COURSE_MAX_AGE_MS = 3000;
    static constexpr uint32_t CAMERA_STALE_GRACE_MS = 10000;
    static constexpr uint32_t CAMERA_STALE_HARD_CLEAR_MS = 30000;
    static constexpr uint32_t CAMERA_DERIVED_COURSE_MAX_AGE_MS = 5000;
    static constexpr float CAMERA_DERIVED_MIN_MOVE_M = 15.0f;
    static constexpr uint16_t CAMERA_STALE_DIVERGE_CLEAR_CM = 6000;
    static constexpr uint8_t CAMERA_STALE_DIVERGE_POLLS = 3;

    void begin(const CameraAlertProviders& providers);

    CameraAlertResult process(uint32_t nowMs, const CameraAlertContext& ctx);

    bool isDisplayActive() const { return displayActive_; }
    bool consumePendingVoice(CameraVoiceEvent& event) const;
    void markVoiceAnnounced(const CameraVoiceEvent& event);
    void resetEncounter();
    bool getStatusSnapshot(CameraAlertStatusSnapshot& snapshot) const;

    static float angleDeltaDeg(float aDeg, float bDeg);
    static bool isAheadByCourse(float courseDeg, float bearingDeg);

private:
    bool isGpsUsable(const CameraAlertContext& ctx) const;
    bool isCameraTypeEnabled(const V1Settings& settings, CameraType type) const;
    void updateEncounterFromResult(uint32_t nowMs, const CameraResult& result, CameraType type);
    void maybeExpireEncounter(uint32_t nowMs, bool sawValidCamera);
    void updateDerivedCourseFromGps(uint32_t nowMs, const GpsRuntimeStatus& gps);
    void resetStaleCourseTracking();
    uint16_t rangeMetersToE5(uint16_t meters) const;
    static float bearingToPointDeg(float fromLatDeg,
                                   float fromLonDeg,
                                   float toLatDeg,
                                   float toLonDeg);
    static float distanceBetweenPointsM(float fromLatDeg,
                                        float fromLonDeg,
                                        float toLatDeg,
                                        float toLonDeg);
    static int32_t degToE5(float deg);

    CameraAlertProviders providers_{};
    CameraResult cachedNearest_{};
    uint32_t lastPollMs_ = 0;
    bool hasPolled_ = false;

    bool encounterActive_ = false;
    int32_t encounterLatE5_ = 0;
    int32_t encounterLonE5_ = 0;
    uint8_t encounterFlags_ = 0;
    CameraType encounterType_ = CameraType::SPEED;
    uint32_t encounterLastSeenMs_ = 0;
    bool farAnnounced_ = false;
    bool nearAnnounced_ = false;
    bool pendingFar_ = false;
    bool pendingNear_ = false;

    bool displayActive_ = false;
    bool hasPayload_ = false;
    CameraAlertDisplayPayload payload_{};

    bool staleCourseActive_ = false;
    uint32_t staleCourseStartMs_ = 0;
    uint16_t staleBestDistanceCm_ = 0xFFFF;
    uint8_t staleDivergePolls_ = 0;

    bool lastGpsValid_ = false;
    float lastGpsLatDeg_ = 0.0f;
    float lastGpsLonDeg_ = 0.0f;
    bool derivedCourseValid_ = false;
    float derivedCourseDeg_ = 0.0f;
    uint32_t derivedCourseTsMs_ = 0;
};
