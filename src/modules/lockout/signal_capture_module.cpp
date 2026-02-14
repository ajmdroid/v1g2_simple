#include "signal_capture_module.h"
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
    lastValid_ = false;
    lastSample_ = SignalObservation{};
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

bool SignalCaptureModule::shouldPublish(const SignalObservation& sample) const {
    if (!lastValid_) {
        return true;
    }

    if (!sameObservationBucket(sample, lastSample_)) {
        return true;
    }

    return static_cast<uint32_t>(sample.tsMs - lastSample_.tsMs) >= SIGNAL_OBS_MIN_REPEAT_MS;
}

void SignalCaptureModule::capturePriorityObservation(uint32_t nowMs,
                                                     const PacketParser& parser,
                                                     const GpsRuntimeStatus& gpsStatus) {
    if (!parser.hasAlerts()) {
        return;
    }

    const AlertData priority = parser.getPriorityAlert();
    if (!priority.isValid || priority.band == BAND_NONE) {
        return;
    }

    SignalObservation observation;
    observation.tsMs = nowMs;
    observation.bandRaw = static_cast<uint8_t>(priority.band);
    observation.strength = std::max(priority.frontStrength, priority.rearStrength);
    observation.frequencyMHz = static_cast<uint16_t>(std::min<uint32_t>(priority.frequency, UINT16_MAX));
    observation.hasFix = gpsStatus.hasFix;
    observation.fixAgeMs = gpsStatus.fixAgeMs;
    observation.satellites = gpsStatus.satellites;
    observation.hdopX10 = hdopToX10(gpsStatus.hdop);
    const bool locationValid = gpsStatus.locationValid &&
                               std::isfinite(gpsStatus.latitudeDeg) &&
                               std::isfinite(gpsStatus.longitudeDeg);
    observation.locationValid = locationValid;
    if (locationValid) {
        observation.latitudeE5 = degreesToE5(gpsStatus.latitudeDeg);
        observation.longitudeE5 = degreesToE5(gpsStatus.longitudeDeg);
    }

    if (!shouldPublish(observation)) {
        return;
    }

    signalObservationLog.publish(observation);
    signalObservationSdLogger.enqueue(observation);
    lastSample_ = observation;
    lastValid_ = true;
}
