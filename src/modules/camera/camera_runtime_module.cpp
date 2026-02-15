#include "camera_runtime_module.h"

#include "../gps/gps_runtime_module.h"

#include <algorithm>
#include <cmath>
#include <esp_heap_caps.h>
#include <limits>

namespace {
constexpr float kAlertRadiusM = 500.0f;
constexpr float kEarthRadiusM = 6371000.0f;
constexpr float kMinimumMatchSpeedMph = 3.0f;
constexpr uint32_t kMaximumGpsSampleAgeMs = 2000;
constexpr uint32_t kMaximumGpsCourseAgeMs = 2000;
constexpr uint32_t kCooldownMs = 30000;
constexpr float kHeadingEntryMaxDeltaDeg = 35.0f;
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

float normalizeHeadingDeg(float headingDeg) {
    if (!std::isfinite(headingDeg)) {
        return NAN;
    }
    float normalized = std::fmod(headingDeg, 360.0f);
    if (normalized < 0.0f) {
        normalized += 360.0f;
    }
    return normalized;
}

float headingDeltaDeg(float headingA, float headingB) {
    const float a = normalizeHeadingDeg(headingA);
    const float b = normalizeHeadingDeg(headingB);
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return NAN;
    }
    float delta = std::fabs(a - b);
    if (delta > 180.0f) {
        delta = 360.0f - delta;
    }
    return delta;
}

float bearingDeg(float latA, float lonA, float latB, float lonB) {
    if (!std::isfinite(latA) || !std::isfinite(lonA) ||
        !std::isfinite(latB) || !std::isfinite(lonB)) {
        return NAN;
    }
    const float phi1 = degToRad(latA);
    const float phi2 = degToRad(latB);
    const float lambda1 = degToRad(lonA);
    const float lambda2 = degToRad(lonB);
    const float deltaLambda = lambda2 - lambda1;

    const float y = std::sin(deltaLambda) * std::cos(phi2);
    const float x = std::cos(phi1) * std::sin(phi2) -
                    std::sin(phi1) * std::cos(phi2) * std::cos(deltaLambda);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        (std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f)) {
        return NAN;
    }
    const float bearingRad = std::atan2(y, x);
    return normalizeHeadingDeg(bearingRad * (180.0f / kPi));
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
    if (!gpsStatus.courseValid || !std::isfinite(gpsStatus.courseDeg)) {
        return false;
    }
    if (gpsStatus.courseAgeMs == UINT32_MAX || gpsStatus.courseAgeMs > kMaximumGpsCourseAgeMs) {
        return false;
    }
    return gpsStatus.speedMph >= kMinimumMatchSpeedMph;
}

float headingToleranceForRecord(const CameraRecord& record) {
    if (record.toleranceDeg == 0) {
        return kHeadingEntryMaxDeltaDeg;
    }
    const float fromRecord = static_cast<float>(record.toleranceDeg);
    return std::clamp(fromRecord, 5.0f, kHeadingEntryMaxDeltaDeg);
}

bool isHeadingEligible(const GpsRuntimeStatus& gpsStatus,
                       const CameraRecord& record,
                       float& outDeltaDeg) {
    outDeltaDeg = NAN;

    float targetHeadingDeg = NAN;
    float headingToleranceDeg = kHeadingEntryMaxDeltaDeg;
    if (record.bearingTenthsDeg >= 0 && record.bearingTenthsDeg < 3600) {
        targetHeadingDeg = static_cast<float>(record.bearingTenthsDeg) * 0.1f;
        headingToleranceDeg = headingToleranceForRecord(record);
    } else {
        targetHeadingDeg = bearingDeg(gpsStatus.latitudeDeg,
                                      gpsStatus.longitudeDeg,
                                      record.latitudeDeg,
                                      record.longitudeDeg);
    }

    const float deltaDeg = headingDeltaDeg(gpsStatus.courseDeg, targetHeadingDeg);
    if (!std::isfinite(deltaDeg)) {
        return false;
    }
    outDeltaDeg = deltaDeg;
    return deltaDeg <= headingToleranceDeg;
}

bool isCameraCoolingDown(const CameraEventLog& eventLog, uint32_t cameraId, uint32_t nowMs) {
    CameraEvent recent[CameraEventLog::kCapacity] = {};
    const size_t count = eventLog.copyRecent(recent, CameraEventLog::kCapacity);
    for (size_t i = 0; i < count; ++i) {
        const CameraEvent& event = recent[i];
        // copyRecent() is newest-first, so once we hit a stale event we can stop.
        const uint32_t ageMs = static_cast<uint32_t>(nowMs - event.tsMs);
        if (ageMs >= kCooldownMs) {
            break;
        }
        if (event.cameraId == cameraId) {
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
    lastHeadingDeltaDeg_ = NAN;
    lastInternalFree_ = 0;
    lastInternalLargestBlock_ = 0;
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
    lastHeadingDeltaDeg_ = NAN;
    lastInternalFree_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lastInternalLargestBlock_ =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (lastInternalFree_ < kMemoryGuardMinFreeInternal ||
        lastInternalLargestBlock_ < kMemoryGuardMinLargestBlock) {
        counters_.cameraTickSkipsMemoryGuard++;
        finalizeTickTiming();
        return;
    }

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
    float bestHeadingDeltaDeg = NAN;

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

                float deltaDeg = NAN;
                if (!isHeadingEligible(gpsStatus, record, deltaDeg)) {
                    continue;
                }

                matchesThisTick++;
                if (distanceM < bestDistanceM) {
                    bestDistanceM = distanceM;
                    bestCameraId = idx + 1u;
                    bestCameraType = record.type;
                    bestHeadingDeltaDeg = deltaDeg;
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
    lastHeadingDeltaDeg_ = bestHeadingDeltaDeg;
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
    out.lastHeadingDeltaDeg = lastHeadingDeltaDeg_;
    out.lastInternalFree = lastInternalFree_;
    out.lastInternalLargestBlock = lastInternalLargestBlock_;
    out.memoryGuardMinFree = kMemoryGuardMinFreeInternal;
    out.memoryGuardMinLargestBlock = kMemoryGuardMinLargestBlock;
    out.counters = counters_;
    out.loader = dataLoader_.status();
    out.counters.cameraLoadFailures = out.loader.loadFailures;
    out.counters.cameraLoadSkipsMemoryGuard = out.loader.loadSkipsMemoryGuard;
    return out;
}
