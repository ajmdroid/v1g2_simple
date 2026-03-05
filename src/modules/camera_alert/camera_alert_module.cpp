#include "camera_alert_module.h"

#include <cmath>

#ifndef UNIT_TEST
#include "modules/gps/gps_runtime_module.h"
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kMetersPerE5 = 1.11f;
constexpr float kMetersToMiles = 0.0006213712f;

}  // namespace

void CameraAlertModule::begin(const CameraAlertProviders& providers) {
    providers_ = providers;
    cachedNearest_ = CameraResult{};
    lastPollMs_ = 0;
    hasPolled_ = false;
    resetStaleCourseTracking();
    lastGpsValid_ = false;
    derivedCourseValid_ = false;
    derivedCourseTsMs_ = 0;
    resetEncounter();
}

float CameraAlertModule::angleDeltaDeg(float aDeg, float bDeg) {
    float delta = fmodf(fabsf(aDeg - bDeg), 360.0f);
    if (delta > 180.0f) {
        delta = 360.0f - delta;
    }
    return delta;
}

bool CameraAlertModule::isAheadByCourse(float courseDeg, float bearingDeg) {
    return angleDeltaDeg(courseDeg, bearingDeg) <= 90.0f;
}

bool CameraAlertModule::consumePendingVoice(CameraVoiceEvent& event) const {
    if (!encounterActive_) {
        return false;
    }
    if (pendingNear_) {
        event.type = encounterType_;
        event.isNearStage = true;
        return true;
    }
    if (pendingFar_) {
        event.type = encounterType_;
        event.isNearStage = false;
        return true;
    }
    return false;
}

void CameraAlertModule::markVoiceAnnounced(const CameraVoiceEvent& event) {
    if (!encounterActive_) {
        return;
    }
    if (event.isNearStage) {
        pendingNear_ = false;
        nearAnnounced_ = true;
    } else {
        pendingFar_ = false;
        farAnnounced_ = true;
    }
}

void CameraAlertModule::resetEncounter() {
    encounterActive_ = false;
    encounterLatE5_ = 0;
    encounterLonE5_ = 0;
    encounterFlags_ = 0;
    encounterType_ = CameraType::SPEED;
    encounterLastSeenMs_ = 0;
    farAnnounced_ = false;
    nearAnnounced_ = false;
    pendingFar_ = false;
    pendingNear_ = false;
    displayActive_ = false;
    hasPayload_ = false;
    payload_ = CameraAlertDisplayPayload{};
}

void CameraAlertModule::resetStaleCourseTracking() {
    staleCourseActive_ = false;
    staleCourseStartMs_ = 0;
    staleBestDistanceCm_ = 0xFFFF;
    staleDivergePolls_ = 0;
}

bool CameraAlertModule::getStatusSnapshot(CameraAlertStatusSnapshot& snapshot) const {
    snapshot.encounterActive = encounterActive_;
    snapshot.displayActive = displayActive_;
    snapshot.pendingFar = pendingFar_;
    snapshot.pendingNear = pendingNear_;
    snapshot.farAnnounced = farAnnounced_;
    snapshot.nearAnnounced = nearAnnounced_;
    snapshot.hasPayload = hasPayload_;
    snapshot.distanceCm = hasPayload_ ? payload_.distanceCm : 0xFFFF;
    snapshot.flags = encounterFlags_;
    snapshot.typeName = encounterActive_ ? cameraTypeStatusName(encounterType_) : "unknown";
    return true;
}

bool CameraAlertModule::isGpsUsable(const CameraAlertContext& ctx) const {
    if (!ctx.gpsStatus) return false;
    const GpsRuntimeStatus& gps = *ctx.gpsStatus;
    if (!gps.enabled || !gps.stableHasFix || !gps.locationValid) {
        return false;
    }
    if (!std::isfinite(gps.latitudeDeg) || !std::isfinite(gps.longitudeDeg)) {
        return false;
    }
    return true;
}

bool CameraAlertModule::isCameraTypeEnabled(const V1Settings& settings, CameraType type) const {
    switch (type) {
        case CameraType::SPEED:
            return settings.cameraTypeSpeed;
        case CameraType::RED_LIGHT:
            return settings.cameraTypeRedLight;
        case CameraType::BUS_LANE:
            return settings.cameraTypeBusLane;
        case CameraType::ALPR:
            return settings.cameraTypeAlpr;
        default:
            return false;
    }
}

void CameraAlertModule::updateEncounterFromResult(uint32_t nowMs,
                                                  const CameraResult& result,
                                                  CameraType type) {
    const bool sameEncounter = encounterActive_ &&
                               encounterLatE5_ == result.latE5 &&
                               encounterLonE5_ == result.lonE5 &&
                               encounterFlags_ == result.flags;
    if (!sameEncounter) {
        encounterActive_ = true;
        encounterLatE5_ = result.latE5;
        encounterLonE5_ = result.lonE5;
        encounterFlags_ = result.flags;
        encounterType_ = type;
        farAnnounced_ = false;
        nearAnnounced_ = false;
        pendingFar_ = false;
        pendingNear_ = false;
    }
    encounterLastSeenMs_ = nowMs;
}

void CameraAlertModule::maybeExpireEncounter(uint32_t nowMs, bool sawValidCamera) {
    if (sawValidCamera || !encounterActive_) {
        return;
    }
    if (encounterLastSeenMs_ == 0 ||
        static_cast<uint32_t>(nowMs - encounterLastSeenMs_) > ENCOUNTER_EXPIRE_MS) {
        resetEncounter();
    }
}

void CameraAlertModule::updateDerivedCourseFromGps(uint32_t nowMs, const GpsRuntimeStatus& gps) {
    if (!std::isfinite(gps.latitudeDeg) || !std::isfinite(gps.longitudeDeg)) {
        lastGpsValid_ = false;
        return;
    }

    if (lastGpsValid_) {
        const float moveM = distanceBetweenPointsM(
            lastGpsLatDeg_, lastGpsLonDeg_, gps.latitudeDeg, gps.longitudeDeg);
        if (moveM >= CAMERA_DERIVED_MIN_MOVE_M) {
            derivedCourseDeg_ = bearingToPointDeg(
                lastGpsLatDeg_, lastGpsLonDeg_, gps.latitudeDeg, gps.longitudeDeg);
            derivedCourseValid_ = true;
            derivedCourseTsMs_ = nowMs;
        }
    }

    lastGpsValid_ = true;
    lastGpsLatDeg_ = gps.latitudeDeg;
    lastGpsLonDeg_ = gps.longitudeDeg;
}

uint16_t CameraAlertModule::rangeMetersToE5(uint16_t meters) const {
    if (meters == 0) {
        return 0;
    }
    const float asE5 = static_cast<float>(meters) / kMetersPerE5;
    if (asE5 <= 1.0f) return 1;
    if (asE5 >= 65535.0f) return 65535;
    return static_cast<uint16_t>(asE5);
}

float CameraAlertModule::bearingToPointDeg(float fromLatDeg,
                                           float fromLonDeg,
                                           float toLatDeg,
                                           float toLonDeg) {
    const float lat1 = fromLatDeg * kDegToRad;
    const float lat2 = toLatDeg * kDegToRad;
    const float dLon = (toLonDeg - fromLonDeg) * kDegToRad;

    const float y = sinf(dLon) * cosf(lat2);
    const float x = cosf(lat1) * sinf(lat2) - sinf(lat1) * cosf(lat2) * cosf(dLon);
    float bearing = atan2f(y, x) * kRadToDeg;
    if (bearing < 0.0f) {
        bearing += 360.0f;
    }
    return bearing;
}

float CameraAlertModule::distanceBetweenPointsM(float fromLatDeg,
                                                float fromLonDeg,
                                                float toLatDeg,
                                                float toLonDeg) {
    const float dLat = (toLatDeg - fromLatDeg) * kDegToRad;
    const float dLon = (toLonDeg - fromLonDeg) * kDegToRad;
    const float lat1 = fromLatDeg * kDegToRad;
    const float lat2 = toLatDeg * kDegToRad;
    const float a = sinf(dLat / 2.0f) * sinf(dLat / 2.0f) +
                    cosf(lat1) * cosf(lat2) *
                    sinf(dLon / 2.0f) * sinf(dLon / 2.0f);
    const float c = 2.0f * atanf(sqrtf(a) / sqrtf(fmaxf(1.0f - a, 0.0f)));
    return 6371000.0f * c;
}

int32_t CameraAlertModule::degToE5(float deg) {
    return static_cast<int32_t>(lroundf(deg * 100000.0f));
}

CameraAlertResult CameraAlertModule::process(uint32_t nowMs, const CameraAlertContext& ctx) {
    CameraAlertResult result;
    const bool v1Suppressed = ctx.v1SignalPriorityActive || ctx.v1PersistedPriorityActive;
    result.suppressedByV1 = v1Suppressed;
    result.payload = payload_;

    if (!ctx.settings || !ctx.settings->cameraAlertsEnabled) {
        resetStaleCourseTracking();
        lastGpsValid_ = false;
        derivedCourseValid_ = false;
        resetEncounter();
        return result;
    }

    const V1Settings& settings = *ctx.settings;
    if (!isGpsUsable(ctx)) {
        resetStaleCourseTracking();
        lastGpsValid_ = false;
        derivedCourseValid_ = false;
        displayActive_ = false;
        result.displayActive = false;
        maybeExpireEncounter(nowMs, false);
        return result;
    }

    const GpsRuntimeStatus& gps = *ctx.gpsStatus;
    updateDerivedCourseFromGps(nowMs, gps);

    if (!providers_.nearestCamera || !providers_.cameraCount ||
        providers_.cameraCount(providers_.context) == 0) {
        resetStaleCourseTracking();
        displayActive_ = false;
        result.displayActive = false;
        maybeExpireEncounter(nowMs, false);
        return result;
    }

    const uint16_t searchRadiusE5 = rangeMetersToE5(settings.cameraAlertRangeM);
    if (!hasPolled_ ||
        static_cast<uint32_t>(nowMs - lastPollMs_) >= CAMERA_POLL_INTERVAL_MS) {
        cachedNearest_ = providers_.nearestCamera(
            providers_.context,
            degToE5(gps.latitudeDeg),
            degToE5(gps.longitudeDeg),
            searchRadiusE5);
        hasPolled_ = true;
        lastPollMs_ = nowMs;
    }

    if (!cachedNearest_.valid) {
        resetStaleCourseTracking();
        displayActive_ = false;
        result.displayActive = false;
        maybeExpireEncounter(nowMs, false);
        return result;
    }

    CameraType mappedType = CameraType::SPEED;
    if (!cameraTypeFromFlags(cachedNearest_.flags, mappedType) ||
        !isCameraTypeEnabled(settings, mappedType)) {
        resetStaleCourseTracking();
        displayActive_ = false;
        result.displayActive = false;
        maybeExpireEncounter(nowMs, false);
        return result;
    }

    const bool sameEncounter = encounterActive_ &&
                               encounterLatE5_ == cachedNearest_.latE5 &&
                               encounterLonE5_ == cachedNearest_.lonE5 &&
                               encounterFlags_ == cachedNearest_.flags;
    if (!sameEncounter) {
        resetStaleCourseTracking();
    }

    const bool courseFresh = gps.courseValid &&
                             std::isfinite(gps.courseDeg) &&
                             gps.courseAgeMs <= CAMERA_COURSE_MAX_AGE_MS;
    const bool derivedCourseFresh = derivedCourseValid_ &&
                                    static_cast<uint32_t>(nowMs - derivedCourseTsMs_) <=
                                        CAMERA_DERIVED_COURSE_MAX_AGE_MS;

    bool effectiveCourseValid = false;
    float effectiveCourseDeg = 0.0f;
    if (courseFresh) {
        effectiveCourseValid = true;
        effectiveCourseDeg = gps.courseDeg;
        resetStaleCourseTracking();
    } else {
        if (!staleCourseActive_) {
            staleCourseActive_ = true;
            staleCourseStartMs_ = nowMs;
            staleBestDistanceCm_ = cachedNearest_.distanceCm;
            staleDivergePolls_ = 0;
        }

        if (cachedNearest_.distanceCm < staleBestDistanceCm_) {
            staleBestDistanceCm_ = cachedNearest_.distanceCm;
            staleDivergePolls_ = 0;
        }

        if (derivedCourseFresh) {
            effectiveCourseValid = true;
            effectiveCourseDeg = derivedCourseDeg_;
        }

        if (!effectiveCourseValid) {
            const uint32_t staleElapsed = static_cast<uint32_t>(nowMs - staleCourseStartMs_);
            const uint32_t divergeThresholdCm =
                static_cast<uint32_t>(staleBestDistanceCm_) + CAMERA_STALE_DIVERGE_CLEAR_CM;
            if (cachedNearest_.distanceCm > divergeThresholdCm) {
                if (staleDivergePolls_ < 255) {
                    staleDivergePolls_++;
                }
            } else {
                staleDivergePolls_ = 0;
            }

            const bool divergedAway = staleElapsed >= CAMERA_STALE_GRACE_MS &&
                                      staleDivergePolls_ >= CAMERA_STALE_DIVERGE_POLLS;
            const bool staleTimeout = staleElapsed >= CAMERA_STALE_HARD_CLEAR_MS;
            if (divergedAway || staleTimeout) {
                resetEncounter();
                result.displayActive = false;
                return result;
            }
        }
    }

    bool orientationRelevant = true;
    if (effectiveCourseValid && cachedNearest_.bearing != 0xFFFF) {
        orientationRelevant = isAheadByCourse(effectiveCourseDeg,
                                              static_cast<float>(cachedNearest_.bearing));
    }

    bool positionAhead = true;
    if (effectiveCourseValid) {
        const float camLatDeg = static_cast<float>(cachedNearest_.latE5) / 100000.0f;
        const float camLonDeg = static_cast<float>(cachedNearest_.lonE5) / 100000.0f;
        const float toCamera = bearingToPointDeg(gps.latitudeDeg, gps.longitudeDeg, camLatDeg, camLonDeg);
        positionAhead = isAheadByCourse(effectiveCourseDeg, toCamera);
    }

    if (!positionAhead) {
        resetEncounter();
        result.displayActive = false;
        return result;
    }
    if (!orientationRelevant) {
        displayActive_ = false;
        result.displayActive = false;
        maybeExpireEncounter(nowMs, false);
        return result;
    }

    updateEncounterFromResult(nowMs, cachedNearest_, mappedType);

    if (settings.cameraVoiceEnabled && !farAnnounced_ &&
        cachedNearest_.distanceCm <= CAMERA_FAR_THRESHOLD_CM) {
        pendingFar_ = true;
    }
    if (settings.cameraVoiceClose && !nearAnnounced_ &&
        cachedNearest_.distanceCm <= CAMERA_NEAR_THRESHOLD_CM) {
        pendingNear_ = true;
    }

    if (!settings.cameraVoiceEnabled) {
        pendingFar_ = false;
    }
    if (!settings.cameraVoiceClose) {
        pendingNear_ = false;
    }

    payload_.type = mappedType;
    payload_.distanceCm = cachedNearest_.distanceCm;
    hasPayload_ = true;
    result.payload = payload_;

    displayActive_ = !v1Suppressed;
    result.displayActive = displayActive_;
    return result;
}
