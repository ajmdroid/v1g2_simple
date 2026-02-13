#include "gps_runtime_module.h"
#include "modules/speed/speed_source_selector.h"
#include <algorithm>
#include <cmath>

GpsRuntimeModule gpsRuntimeModule;

void GpsRuntimeModule::begin(bool enabled) {
    setEnabled(enabled);
}

void GpsRuntimeModule::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) {
        clearSample();
    }
}

void GpsRuntimeModule::update(uint32_t nowMs) {
    (void)nowMs;
    // Intentionally no-op in stage 2. Hardware/NMEA parser integration lands later.
}

void GpsRuntimeModule::setScaffoldSample(float speedMph,
                                         bool hasFix,
                                         uint8_t satellites,
                                         float hdop,
                                         uint32_t timestampMs) {
    if (!enabled_) {
        return;
    }
    if (!std::isfinite(speedMph)) {
        return;
    }

    sampleValid_ = true;
    hasFix_ = hasFix;
    speedMph_ = std::clamp(speedMph, 0.0f, SpeedSourceSelector::MAX_VALID_SPEED_MPH);
    satellites_ = satellites;
    hdop_ = std::isfinite(hdop) ? std::max(0.0f, hdop) : NAN;
    sampleTsMs_ = (timestampMs == 0) ? millis() : timestampMs;
    injectedSamples_++;
}

void GpsRuntimeModule::clearSample() {
    sampleValid_ = false;
    hasFix_ = false;
    speedMph_ = 0.0f;
    satellites_ = 0;
    hdop_ = NAN;
    sampleTsMs_ = 0;
}

bool GpsRuntimeModule::getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const {
    if (!enabled_ || !sampleValid_ || !hasFix_) {
        return false;
    }
    if (sampleTsMs_ == 0 || nowMs < sampleTsMs_) {
        return false;
    }
    if ((nowMs - sampleTsMs_) > SAMPLE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = speedMph_;
    tsMsOut = sampleTsMs_;
    return true;
}

GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t nowMs) const {
    GpsRuntimeStatus status;
    status.enabled = enabled_;
    status.sampleValid = sampleValid_;
    status.hasFix = hasFix_;
    status.speedMph = speedMph_;
    status.satellites = satellites_;
    status.hdop = hdop_;
    status.sampleTsMs = sampleTsMs_;
    status.injectedSamples = injectedSamples_;

    if (sampleValid_ && sampleTsMs_ != 0 && nowMs >= sampleTsMs_) {
        status.sampleAgeMs = nowMs - sampleTsMs_;
    } else {
        status.sampleAgeMs = UINT32_MAX;
    }

    return status;
}
