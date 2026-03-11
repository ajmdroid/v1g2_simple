#include "camera_alert_module.h"

#include "../lockout/road_map_reader.h"
#include "../../perf_metrics.h"
#include "../../settings.h"

#include <math.h>

namespace {

struct CameraProcessPerfScope {
    unsigned long startUs = 0;

    ~CameraProcessPerfScope() {
        perfRecordCameraProcessUs(micros() - startUs);
    }
};

constexpr uint32_t CAMERA_POLL_INTERVAL_MS = 500;
constexpr uint32_t ENCOUNTER_EXPIRE_MS = 10000;
constexpr uint32_t CAMERA_COURSE_MAX_AGE_MS = 3000;
constexpr float CAMERA_MIN_SPEED_MPH = 15.0f;
constexpr float CAMERA_CORRIDOR_WIDTH_M = 50.0f;
constexpr uint32_t CRUMB_MIN_SEPARATION_CM = 1000;
constexpr uint32_t CLOSING_TOLERANCE_CM = 100;
constexpr float E5_TO_METRES = 1.11f;
constexpr float PI_F = 3.14159265f;

CameraAlertDisplayPayload inactivePayload() {
    CameraAlertDisplayPayload payload;
    payload.active = false;
    payload.distanceCm = CAMERA_DISTANCE_INVALID_CM;
    return payload;
}

float cosLatForE5(int32_t latE5) {
    const float latRad = static_cast<float>(latE5) / 100000.0f * (PI_F / 180.0f);
    return cosf(latRad);
}

float headingToUnitX(float headingDeg) {
    return sinf(headingDeg * (PI_F / 180.0f));
}

float headingToUnitY(float headingDeg) {
    return cosf(headingDeg * (PI_F / 180.0f));
}

float headingDeltaDeg(float aDeg, float bDeg) {
    float diff = fabsf(aDeg - bDeg);
    while (diff >= 360.0f) {
        diff -= 360.0f;
    }
    if (diff > 180.0f) {
        diff = 360.0f - diff;
    }
    return diff;
}

float pointDistanceMetres(int32_t aLatE5, int32_t aLonE5,
                          int32_t bLatE5, int32_t bLonE5) {
    const float cosLat = cosLatForE5((aLatE5 + bLatE5) / 2);
    const float dLatM = static_cast<float>(bLatE5 - aLatE5) * E5_TO_METRES;
    const float dLonM = static_cast<float>(bLonE5 - aLonE5) * E5_TO_METRES * cosLat;
    return sqrtf((dLatM * dLatM) + (dLonM * dLonM));
}

uint32_t pointDistanceCm(int32_t aLatE5, int32_t aLonE5,
                         int32_t bLatE5, int32_t bLonE5) {
    return static_cast<uint32_t>(pointDistanceMetres(aLatE5, aLonE5, bLatE5, bLonE5) * 100.0f);
}

float bearingDeg(int32_t aLatE5, int32_t aLonE5,
                 int32_t bLatE5, int32_t bLonE5) {
    const float dLat = static_cast<float>(bLatE5 - aLatE5);
    const float midLatRad = static_cast<float>(aLatE5 + bLatE5) / 2.0f
                            / 100000.0f * (PI_F / 180.0f);
    const float dLon = static_cast<float>(bLonE5 - aLonE5) * cosf(midLatRad);
    float deg = atan2f(dLon, dLat) * (180.0f / PI_F);
    if (deg < 0.0f) {
        deg += 360.0f;
    }
    return deg;
}

bool cameraInForwardCorridor(int32_t latE5, int32_t lonE5,
                             int32_t cameraLatE5, int32_t cameraLonE5,
                             float headingDeg) {
    const float cosLat = cosLatForE5((latE5 + cameraLatE5) / 2);
    const float dx = static_cast<float>(cameraLonE5 - lonE5) * E5_TO_METRES * cosLat;
    const float dy = static_cast<float>(cameraLatE5 - latE5) * E5_TO_METRES;
    const float fx = headingToUnitX(headingDeg);
    const float fy = headingToUnitY(headingDeg);

    const float forwardDistance = (dx * fx) + (dy * fy);
    if (forwardDistance <= 0.0f) {
        return false;
    }

    const float lateralDistance = fabsf((dx * fy) - (dy * fx));
    return lateralDistance <= CAMERA_CORRIDOR_WIDTH_M;
}

bool cameraBearingMatches(float travelHeadingDeg, uint16_t cameraBearing) {
    if (cameraBearing == 0xFFFF) {
        return true;
    }
    return headingDeltaDeg(travelHeadingDeg, static_cast<float>(cameraBearing)) <= 90.0f;
}

uint16_t searchRadiusE5FromRangeCm(uint32_t rangeCm) {
    const float radiusE5 = ceilf(static_cast<float>(rangeCm) / 111.32f);
    if (radiusE5 <= 0.0f) {
        return 1;
    }
    if (radiusE5 > 65535.0f) {
        return 65535;
    }
    return static_cast<uint16_t>(radiusE5);
}

}  // namespace

CameraAlertModule cameraAlertModule;

void CameraAlertModule::begin(RoadMapReader* roadMap, SettingsManager* settings) {
    roadMap_ = roadMap;
    settings_ = settings;
    clearBreadcrumbs();
    clearEncounterState();
    lastPollMs_ = 0;
    hasPolled_ = false;
}

void CameraAlertModule::clearBreadcrumbs() {
    breadcrumbCount_ = 0;
    breadcrumbWriteIndex_ = 0;
    for (uint8_t i = 0; i < kBreadcrumbCapacity; ++i) {
        breadcrumbs_[i] = Breadcrumb{};
    }
}

void CameraAlertModule::deactivateDisplay() {
    displayPayload_ = inactivePayload();
}

void CameraAlertModule::clearEncounterState() {
    state_ = ApproachState::IDLE;
    encounterLatE5_ = 0;
    encounterLonE5_ = 0;
    encounterFlags_ = 0;
    lastDistanceCm_ = CAMERA_DISTANCE_INVALID_CM;
    lastSeenMs_ = 0;
    closingPollCount_ = 0;
    deactivateDisplay();
}

void CameraAlertModule::resetEncounter() {
    clearEncounterState();
    clearBreadcrumbs();
}

void CameraAlertModule::beginEncounter(const CameraResult& result) {
    state_ = ApproachState::DETECTED;
    encounterLatE5_ = result.latE5;
    encounterLonE5_ = result.lonE5;
    encounterFlags_ = result.flags;
    lastDistanceCm_ = result.distanceCm;
    closingPollCount_ = 0;
    deactivateDisplay();
}

bool CameraAlertModule::matchesEncounter(const CameraResult& result) const {
    return state_ != ApproachState::IDLE &&
           encounterLatE5_ == result.latE5 &&
           encounterLonE5_ == result.lonE5 &&
           encounterFlags_ == result.flags;
}

void CameraAlertModule::recordBreadcrumb(const CameraAlertContext& ctx) {
    if (!ctx.gpsValid || ctx.speedMph < CAMERA_MIN_SPEED_MPH) {
        return;
    }

    if (breadcrumbCount_ > 0) {
        const uint8_t lastIndex =
            static_cast<uint8_t>((breadcrumbWriteIndex_ + kBreadcrumbCapacity - 1) % kBreadcrumbCapacity);
        const Breadcrumb& last = breadcrumbs_[lastIndex];
        if (pointDistanceCm(last.latE5, last.lonE5, ctx.latE5, ctx.lonE5) < CRUMB_MIN_SEPARATION_CM) {
            return;
        }
    }

    breadcrumbs_[breadcrumbWriteIndex_].latE5 = ctx.latE5;
    breadcrumbs_[breadcrumbWriteIndex_].lonE5 = ctx.lonE5;
    breadcrumbWriteIndex_ = static_cast<uint8_t>((breadcrumbWriteIndex_ + 1) % kBreadcrumbCapacity);
    if (breadcrumbCount_ < kBreadcrumbCapacity) {
        ++breadcrumbCount_;
    }
}

bool CameraAlertModule::resolveTravelHeadingDeg(const CameraAlertContext& ctx, float& headingDeg) const {
    if (breadcrumbCount_ >= 2) {
        const uint8_t oldestIndex =
            static_cast<uint8_t>((breadcrumbWriteIndex_ + kBreadcrumbCapacity - breadcrumbCount_) %
                                 kBreadcrumbCapacity);
        const uint8_t newestIndex =
            static_cast<uint8_t>((breadcrumbWriteIndex_ + kBreadcrumbCapacity - 1) % kBreadcrumbCapacity);
        const Breadcrumb& oldest = breadcrumbs_[oldestIndex];
        const Breadcrumb& newest = breadcrumbs_[newestIndex];
        if (pointDistanceCm(oldest.latE5, oldest.lonE5, newest.latE5, newest.lonE5) >=
            CRUMB_MIN_SEPARATION_CM) {
            headingDeg = bearingDeg(oldest.latE5, oldest.lonE5, newest.latE5, newest.lonE5);
            return true;
        }
    }

    if (ctx.courseValid && ctx.courseAgeMs <= CAMERA_COURSE_MAX_AGE_MS) {
        headingDeg = ctx.courseDeg;
        return true;
    }

    return false;
}

void CameraAlertModule::expireEncounterIfNeeded(uint32_t nowMs) {
    if (state_ == ApproachState::IDLE) {
        return;
    }
    if ((nowMs - lastSeenMs_) > ENCOUNTER_EXPIRE_MS) {
        clearEncounterState();
    }
}

void CameraAlertModule::process(uint32_t nowMs, const CameraAlertContext& ctx) {
    CameraProcessPerfScope perfScope{micros()};
    if (hasPolled_ && (nowMs - lastPollMs_) < CAMERA_POLL_INTERVAL_MS) {
        return;
    }
    lastPollMs_ = nowMs;
    hasPolled_ = true;

    if (!roadMap_ || !settings_ || !roadMap_->isLoaded() || roadMap_->cameraCount() == 0) {
        resetEncounter();
        return;
    }

    const V1Settings& settings = settings_->get();
    if (!settings.cameraAlertsEnabled || ctx.speedMph < CAMERA_MIN_SPEED_MPH || !ctx.gpsValid) {
        resetEncounter();
        return;
    }

    recordBreadcrumb(ctx);

    const uint32_t rangeCm = clampCameraAlertRangeCmValue(static_cast<int>(settings.cameraAlertRangeCm));
    const uint16_t searchRadiusE5 = searchRadiusE5FromRangeCm(rangeCm);
    const CameraResult result = roadMap_->nearestCamera(ctx.latE5, ctx.lonE5, searchRadiusE5);
    if (!result.valid || result.distanceCm == CAMERA_DISTANCE_INVALID_CM || result.distanceCm > rangeCm) {
        deactivateDisplay();
        expireEncounterIfNeeded(nowMs);
        return;
    }

    if (cameraTypeFromFlags(result.flags) == CameraType::INVALID) {
        deactivateDisplay();
        expireEncounterIfNeeded(nowMs);
        return;
    }

    float headingDeg = 0.0f;
    const bool haveHeading = resolveTravelHeadingDeg(ctx, headingDeg);
    if (haveHeading) {
        if (!cameraInForwardCorridor(ctx.latE5, ctx.lonE5, result.latE5, result.lonE5, headingDeg) ||
            !cameraBearingMatches(headingDeg, result.bearing)) {
            if (matchesEncounter(result)) {
                state_ = ApproachState::DETECTED;
                closingPollCount_ = 0;
                lastDistanceCm_ = result.distanceCm;
            }
            deactivateDisplay();
            expireEncounterIfNeeded(nowMs);
            return;
        }
    }

    if (!matchesEncounter(result)) {
        beginEncounter(result);
        lastSeenMs_ = nowMs;
        return;
    }

    const bool movingAway =
        (lastDistanceCm_ != CAMERA_DISTANCE_INVALID_CM) &&
        (result.distanceCm > (lastDistanceCm_ + CLOSING_TOLERANCE_CM));
    const bool closing =
        (lastDistanceCm_ == CAMERA_DISTANCE_INVALID_CM) ||
        (result.distanceCm + CLOSING_TOLERANCE_CM < lastDistanceCm_);

    if (movingAway) {
        state_ = ApproachState::DETECTED;
        closingPollCount_ = 0;
        lastDistanceCm_ = result.distanceCm;
        deactivateDisplay();
        expireEncounterIfNeeded(nowMs);
        return;
    }

    lastSeenMs_ = nowMs;
    lastDistanceCm_ = result.distanceCm;

    if (closing) {
        if (state_ == ApproachState::DETECTED) {
            state_ = ApproachState::APPROACHING;
            closingPollCount_ = 1;
        } else if (state_ == ApproachState::APPROACHING) {
            ++closingPollCount_;
            if (closingPollCount_ >= 2) {
                state_ = ApproachState::CONFIRMED;
            }
        }
    }

    if (state_ == ApproachState::CONFIRMED) {
        displayPayload_.active = true;
        displayPayload_.distanceCm = result.distanceCm;
        return;
    }

    deactivateDisplay();
}
