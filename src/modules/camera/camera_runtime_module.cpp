#include "camera_runtime_module.h"

#include "../gps/gps_runtime_module.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kAlertRadiusM = 500.0f;
constexpr float kEarthRadiusM = 6371000.0f;
constexpr float kMinimumMatchSpeedMph = 3.0f;
constexpr uint32_t kMaximumGpsSampleAgeMs = 2000;
constexpr uint32_t kCooldownMs = 30000;
constexpr float kPi = 3.14159265358979323846f;

float degToRad(float value) {
    return value * (kPi / 180.0f);
}

float haversineMeters(float latA, float lonA, float latB, float lonB) {
    const float phi1 = degToRad(latA);
    const float phi2 = degToRad(latB);
    const float deltaPhi = degToRad(latB - latA);
    const float deltaLambda = degToRad(lonB - lonA);
    const float sinHalfPhi = std::sin(deltaPhi * 0.5f);
    const float sinHalfLambda = std::sin(deltaLambda * 0.5f);
    const float a = sinHalfPhi * sinHalfPhi +
                    std::cos(phi1) * std::cos(phi2) * sinHalfLambda * sinHalfLambda;
    const float c = 2.0f * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0f, 1.0f - a)));
    return kEarthRadiusM * c;
}

const CameraCellSpan* findCellSpan(const CameraCellSpan* spans, uint32_t spanCount, uint32_t cellKey) {
    uint32_t lo = 0;
    uint32_t hi = spanCount;
    while (lo < hi) {
        const uint32_t mid = lo + ((hi - lo) / 2u);
        const uint32_t midKey = spans[mid].cellKey;
        if (midKey < cellKey) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    if (lo < spanCount && spans[lo].cellKey == cellKey) {
        return &spans[lo];
    }
    return nullptr;
}

bool isGpsEligibleForCameraMatch(const GpsRuntimeStatus& gpsStatus) {
    if (!gpsStatus.enabled || !gpsStatus.hasFix || !gpsStatus.locationValid) {
        return false;
    }
    if (!std::isfinite(gpsStatus.latitudeDeg) || !std::isfinite(gpsStatus.longitudeDeg)) {
        return false;
    }
    if (gpsStatus.sampleAgeMs == UINT32_MAX || gpsStatus.sampleAgeMs > kMaximumGpsSampleAgeMs) {
        return false;
    }
    return gpsStatus.speedMph >= kMinimumMatchSpeedMph;
}

bool isCameraCoolingDown(const CameraEventLog& eventLog, uint32_t cameraId, uint32_t nowMs) {
    CameraEvent recent[CameraEventLog::kCapacity] = {};
    const size_t count = eventLog.copyRecent(recent, CameraEventLog::kCapacity);
    for (size_t i = 0; i < count; ++i) {
        const CameraEvent& event = recent[i];
        if (event.cameraId != cameraId) {
            continue;
        }
        if (static_cast<uint32_t>(nowMs - event.tsMs) < kCooldownMs) {
            return true;
        }
    }
    return false;
}

uint16_t toDistanceMeters(float distanceM) {
    if (!std::isfinite(distanceM) || distanceM <= 0.0f) {
        return 0;
    }
    const float clamped = std::min(distanceM, static_cast<float>(std::numeric_limits<uint16_t>::max()));
    return static_cast<uint16_t>(clamped);
}
}  // namespace

CameraRuntimeModule cameraRuntimeModule;

void CameraRuntimeModule::begin(bool enabled) {
    enabled_ = enabled;
    lastTickMs_ = 0;
    lastTickDurationUs_ = 0;
    maxTickDurationUs_ = 0;
    lastCandidatesChecked_ = 0;
    lastMatches_ = 0;
    lastCapReached_ = false;
    counters_ = {};
    index_.clear();
    eventLog_.reset();
    dataLoader_.reset();
    dataLoader_.begin();
    if (enabled_) {
        dataLoader_.requestReload();
    }
}

void CameraRuntimeModule::setEnabled(bool enabled) {
    const bool wasEnabled = enabled_;
    enabled_ = enabled;
    if (enabled_ && !wasEnabled && !index_.isLoaded()) {
        dataLoader_.requestReload();
    }
}

bool CameraRuntimeModule::tryLoadDefault(uint32_t nowMs) {
    (void)nowMs;
    dataLoader_.requestReload();
    return index_.isLoaded();
}

void CameraRuntimeModule::requestReload() {
    dataLoader_.requestReload();
}

void CameraRuntimeModule::process(uint32_t nowMs, bool skipNonCoreThisLoop, bool overloadThisLoop) {
    if (dataLoader_.consumeReady(index_)) {
        counters_.cameraIndexSwapCount++;
    }

    if (!enabled_) {
        return;
    }

    if (skipNonCoreThisLoop) {
        counters_.cameraTickSkipsNonCore++;
        return;
    }

    if (overloadThisLoop) {
        counters_.cameraTickSkipsOverload++;
        return;
    }

    if (lastTickMs_ != 0 && static_cast<uint32_t>(nowMs - lastTickMs_) < tickIntervalMs_) {
        return;
    }
    lastTickMs_ = nowMs;
    counters_.cameraTicks++;
    const uint32_t tickStartUs = micros();
    auto finalizeTickTiming = [this, tickStartUs]() {
        const uint32_t durationUs = static_cast<uint32_t>(micros() - tickStartUs);
        lastTickDurationUs_ = durationUs;
        if (durationUs > maxTickDurationUs_) {
            maxTickDurationUs_ = durationUs;
        }
    };
    lastCandidatesChecked_ = 0;
    lastMatches_ = 0;
    lastCapReached_ = false;

    if (!index_.isLoaded()) {
        finalizeTickTiming();
        return;
    }

    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    if (!isGpsEligibleForCameraMatch(gpsStatus)) {
        finalizeTickTiming();
        return;
    }

    const CameraRecord* records = index_.records();
    const CameraCellSpan* spans = index_.spans();
    const uint32_t spanCount = index_.bucketCount();
    if (!records || !spans || spanCount == 0) {
        finalizeTickTiming();
        return;
    }

    const int32_t latitudeCell =
        static_cast<int32_t>(std::floor(gpsStatus.latitudeDeg / CameraIndex::kCellSizeDeg));
    const int32_t longitudeCell =
        static_cast<int32_t>(std::floor(gpsStatus.longitudeDeg / CameraIndex::kCellSizeDeg));

    uint32_t visitedCandidates = 0;
    uint32_t matchesThisTick = 0;
    bool capReached = false;
    uint32_t bestCameraId = 0;
    float bestDistanceM = std::numeric_limits<float>::max();
    uint8_t bestCameraType = 0;

    for (int32_t latDelta = -1; latDelta <= 1; ++latDelta) {
        for (int32_t lonDelta = -1; lonDelta <= 1; ++lonDelta) {
            if (visitedCandidates >= CameraIndex::kRawScanCap) {
                capReached = true;
                break;
            }

            const uint32_t cellKey =
                CameraIndex::encodeCellKeyFromCell(latitudeCell + latDelta, longitudeCell + lonDelta);
            const CameraCellSpan* span = findCellSpan(spans, spanCount, cellKey);
            if (!span) {
                continue;
            }

            for (uint32_t idx = span->beginIndex; idx < span->endIndex; ++idx) {
                if (visitedCandidates >= CameraIndex::kRawScanCap) {
                    capReached = true;
                    break;
                }

                visitedCandidates++;
                const CameraRecord& record = records[idx];
                const float distanceM = haversineMeters(gpsStatus.latitudeDeg,
                                                        gpsStatus.longitudeDeg,
                                                        record.latitudeDeg,
                                                        record.longitudeDeg);
                if (!std::isfinite(distanceM) || distanceM > kAlertRadiusM) {
                    continue;
                }

                matchesThisTick++;
                if (distanceM < bestDistanceM) {
                    bestDistanceM = distanceM;
                    bestCameraId = idx + 1u;
                    bestCameraType = record.type;
                }
            }
        }
        if (capReached) {
            break;
        }
    }

    counters_.cameraCandidatesChecked += visitedCandidates;
    counters_.cameraMatches += matchesThisTick;
    lastCandidatesChecked_ = visitedCandidates;
    lastMatches_ = matchesThisTick;
    lastCapReached_ = capReached;
    if (capReached) {
        counters_.cameraBudgetExceeded++;
    }

    if (bestCameraId == 0 || isCameraCoolingDown(eventLog_, bestCameraId, nowMs)) {
        finalizeTickTiming();
        return;
    }

    CameraEvent event;
    event.tsMs = nowMs;
    event.cameraId = bestCameraId;
    event.distanceM = toDistanceMeters(bestDistanceM);
    event.type = bestCameraType;
    event.synthetic = false;
    if (eventLog_.publish(event)) {
        counters_.cameraAlertsStarted++;
    }
    finalizeTickTiming();
}

CameraRuntimeStatus CameraRuntimeModule::snapshot() const {
    CameraRuntimeStatus out;
    out.enabled = enabled_;
    out.indexLoaded = index_.isLoaded();
    out.tickIntervalMs = tickIntervalMs_;
    out.lastTickMs = lastTickMs_;
    out.lastTickDurationUs = lastTickDurationUs_;
    out.maxTickDurationUs = maxTickDurationUs_;
    out.lastCandidatesChecked = lastCandidatesChecked_;
    out.lastMatches = lastMatches_;
    out.lastCapReached = lastCapReached_;
    out.counters = counters_;
    out.loader = dataLoader_.status();
    out.counters.cameraLoadFailures = out.loader.loadFailures;
    return out;
}
