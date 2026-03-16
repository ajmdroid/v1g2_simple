#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "signal_observation_log.h"
#include "../speed/speed_source_selector.h"

class PacketParser;
struct GpsRuntimeStatus;

// Captures bounded lockout-candidate observations from the current V1 alert set.
class SignalCaptureModule {
public:
    void reset();
    void capturePriorityObservation(uint32_t nowMs,
                                    const PacketParser& parser,
                                    const GpsRuntimeStatus& gpsStatus,
                                    bool captureUnsupportedBandsToSd = false);
    void capturePriorityObservation(uint32_t nowMs,
                                    const PacketParser& parser,
                                    const GpsRuntimeStatus& gpsStatus,
                                    const SpeedSelection& selectedSpeed,
                                    bool captureUnsupportedBandsToSd = false);

private:
    struct RecentBucket {
        SignalObservation observation = {};
        bool valid = false;
    };

    static constexpr size_t kRecentBucketCount = 16;

    static int32_t degreesToE5(float degrees);
    static uint16_t hdopToX10(float hdop);
    static bool sameObservationBucket(const SignalObservation& a, const SignalObservation& b);
    bool shouldPublish(const SignalObservation& sample, size_t* matchedBucketIndex) const;
    void rememberPublishedObservation(const SignalObservation& sample, size_t matchedBucketIndex);

    RecentBucket recentBuckets_[kRecentBucketCount] = {};
    size_t nextRecentBucketIndex_ = 0;
};

extern SignalCaptureModule signalCaptureModule;
