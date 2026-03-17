#include "signal_capture_module.h"
#include "lockout_band_policy.h"
#ifdef UNIT_TEST
class SignalObservationSdLogger {
public:
    bool enqueue(const SignalObservation& observation);
};
extern SignalObservationSdLogger signalObservationSdLogger;
#else
#include "signal_observation_sd_logger.h"
#endif

#include "../../packet_parser.h"
#include "../gps/gps_runtime_module.h"

#include <algorithm>
#include <cmath>

namespace {
// Signal-observation capture guardrails.
// Keeps records useful for lockout learning while bounding write volume.
constexpr uint32_t SIGNAL_OBS_MIN_REPEAT_MS = 1500;
constexpr uint16_t SIGNAL_OBS_FREQ_TOL_MHZ = 5;
constexpr uint8_t SIGNAL_OBS_STRENGTH_TOL = 1;
constexpr int32_t SIGNAL_OBS_LOCATION_TOL_E5 = 25;  // ~28m latitude
}  // namespace

SignalCaptureModule signalCaptureModule;

void SignalCaptureModule::reset() {
    for (size_t i = 0; i < kRecentBucketCount; ++i) {
        recentBuckets_[i].valid = false;
    }
    nextRecentBucketIndex_ = 0;
}

int32_t SignalCaptureModule::degreesToE5(float degrees) {
    return static_cast<int32_t>(lroundf(degrees * 100000.0f));
}

uint16_t SignalCaptureModule::hdopToX10(float hdop) {
    if (!std::isfinite(hdop) || hdop < 0.0f) {
        return SignalObservation::HDOP_X10_INVALID;
    }
    const long scaled = lroundf(hdop * 10.0f);
    if (scaled < 0 || scaled > static_cast<long>(UINT16_MAX - 1)) {
        return SignalObservation::HDOP_X10_INVALID;
    }
    return static_cast<uint16_t>(scaled);
}

bool SignalCaptureModule::sameObservationBucket(const SignalObservation& a,
                                                const SignalObservation& b) {
    if (a.bandRaw != b.bandRaw) {
        return false;
    }

    const int freqDiff = abs(static_cast<int>(a.frequencyMHz) - static_cast<int>(b.frequencyMHz));
    if (freqDiff > SIGNAL_OBS_FREQ_TOL_MHZ) {
        return false;
    }

    const int strengthDiff = abs(static_cast<int>(a.strength) - static_cast<int>(b.strength));
    if (strengthDiff > SIGNAL_OBS_STRENGTH_TOL) {
        return false;
    }

    if (a.locationValid != b.locationValid) {
        return false;
    }

    if (!a.locationValid) {
        return true;
    }

    const int32_t latDiff = abs(a.latitudeE5 - b.latitudeE5);
    const int32_t lonDiff = abs(a.longitudeE5 - b.longitudeE5);
    return latDiff <= SIGNAL_OBS_LOCATION_TOL_E5 && lonDiff <= SIGNAL_OBS_LOCATION_TOL_E5;
}

bool SignalCaptureModule::shouldPublish(const SignalObservation& sample,
                                        size_t* matchedBucketIndex) const {
    if (matchedBucketIndex) {
        *matchedBucketIndex = kRecentBucketCount;
    }

    for (size_t i = 0; i < kRecentBucketCount; ++i) {
        const RecentBucket& bucket = recentBuckets_[i];
        if (!bucket.valid) {
            continue;
        }
        if (!sameObservationBucket(sample, bucket.observation)) {
            continue;
        }
        if (matchedBucketIndex) {
            *matchedBucketIndex = i;
        }
        const uint32_t elapsedMs =
            static_cast<uint32_t>(sample.tsMs - bucket.observation.tsMs);
        return elapsedMs >= SIGNAL_OBS_MIN_REPEAT_MS;
    }

    return true;
}

void SignalCaptureModule::rememberPublishedObservation(const SignalObservation& sample,
                                                       size_t matchedBucketIndex) {
    size_t bucketIndex = matchedBucketIndex;
    if (bucketIndex >= kRecentBucketCount) {
        bucketIndex = nextRecentBucketIndex_;
        nextRecentBucketIndex_ = (nextRecentBucketIndex_ + 1) % kRecentBucketCount;
    }

    recentBuckets_[bucketIndex].observation = sample;
    recentBuckets_[bucketIndex].valid = true;
}

void SignalCaptureModule::capturePriorityObservation(uint32_t nowMs,
                                                     const PacketParser& parser,
                                                     const GpsRuntimeStatus& gpsStatus,
                                                     bool captureUnsupportedBandsToSd) {
    capturePriorityObservation(nowMs,
                               parser,
                               gpsStatus,
                               SpeedSelection{},
                               captureUnsupportedBandsToSd);
}

void SignalCaptureModule::capturePriorityObservation(uint32_t nowMs,
                                                     const PacketParser& parser,
                                                     const GpsRuntimeStatus& gpsStatus,
                                                     const SpeedSelection& selectedSpeed,
                                                     bool captureUnsupportedBandsToSd) {
    if (!parser.hasAlerts()) {
        return;
    }

    const bool locationValid = gpsStatus.locationValid &&
                               std::isfinite(gpsStatus.latitudeDeg) &&
                               std::isfinite(gpsStatus.longitudeDeg);
    const int32_t latitudeE5 = locationValid ? degreesToE5(gpsStatus.latitudeDeg) : 0;
    const int32_t longitudeE5 = locationValid ? degreesToE5(gpsStatus.longitudeDeg) : 0;
    SignalObservation publishedObservations[PacketParser::MAX_ALERTS];
    size_t publishedBucketIndices[PacketParser::MAX_ALERTS];
    size_t publishedCount = 0;

    const auto& alerts = parser.getAllAlerts();
    const size_t alertCount = static_cast<size_t>(parser.getAlertCount());
    for (size_t i = 0; i < alertCount; ++i) {
        const AlertData& alert = alerts[i];
        if (!alert.isValid || alert.band == BAND_NONE) {
            continue;
        }

        const uint8_t bandRaw = static_cast<uint8_t>(alert.band);
        const bool bandSupportedForLockout = lockoutBandSupported(bandRaw);
        if (!bandSupportedForLockout && !captureUnsupportedBandsToSd) {
            continue;
        }

        SignalObservation observation;
        observation.tsMs = nowMs;
        observation.bandRaw = bandRaw;
        observation.strength = std::max(alert.frontStrength, alert.rearStrength);
        observation.frequencyMHz = static_cast<uint16_t>(
            std::min<uint32_t>(alert.frequency, UINT16_MAX));
        observation.hasFix = gpsStatus.hasFix;
        observation.fixAgeMs = gpsStatus.fixAgeMs;
        observation.satellites = gpsStatus.satellites;
        observation.hdopX10 = hdopToX10(gpsStatus.hdop);
        if (selectedSpeed.valid) {
            observation.speedMph = selectedSpeed.speedMph;
            observation.speedSourceRaw = static_cast<uint8_t>(selectedSpeed.source);
        }
        observation.courseValid = gpsStatus.courseValid;
        observation.courseDeg = gpsStatus.courseDeg;
        observation.locationValid = locationValid;
        if (locationValid) {
            observation.latitudeE5 = latitudeE5;
            observation.longitudeE5 = longitudeE5;
        }

        size_t matchedBucketIndex = kRecentBucketCount;
        if (!shouldPublish(observation, &matchedBucketIndex)) {
            continue;
        }

        if (bandSupportedForLockout) {
            signalObservationLog.publish(observation);
        }
        signalObservationSdLogger.enqueue(observation);
        if (publishedCount < PacketParser::MAX_ALERTS) {
            publishedObservations[publishedCount] = observation;
            publishedBucketIndices[publishedCount] = matchedBucketIndex;
            ++publishedCount;
        }
    }

    for (size_t i = 0; i < publishedCount; ++i) {
        rememberPublishedObservation(publishedObservations[i], publishedBucketIndices[i]);
    }
}
