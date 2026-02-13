#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "signal_observation_log.h"

class PacketParser;
struct GpsRuntimeStatus;

// Captures bounded lockout-candidate observations from parsed priority alerts.
class SignalCaptureModule {
public:
    void reset();
    void capturePriorityObservation(uint32_t nowMs,
                                    const PacketParser& parser,
                                    const GpsRuntimeStatus& gpsStatus);

private:
    static int32_t degreesToE5(float degrees);
    static uint16_t hdopToX10(float hdop);
    static bool sameObservationBucket(const SignalObservation& a, const SignalObservation& b);
    bool shouldPublish(const SignalObservation& sample) const;

    bool lastValid_ = false;
    SignalObservation lastSample_ = {};
};

extern SignalCaptureModule signalCaptureModule;

