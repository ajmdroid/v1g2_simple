#include "speed_source_selector.h"
#include <cmath>

SpeedSourceSelector speedSourceSelector;

void SpeedSourceSelector::begin(bool gpsEnabled) {
    gpsEnabled_ = gpsEnabled;
    obdConnected_ = false;
    selectedSource_ = SpeedSource::NONE;
}

void SpeedSourceSelector::setGpsEnabled(bool enabled) {
    gpsEnabled_ = enabled;
    if (!gpsEnabled_) {
        gps_.valid = false;
        if (selectedSource_ == SpeedSource::GPS) {
            selectedSource_ = SpeedSource::NONE;
        }
    }
}

void SpeedSourceSelector::setObdConnected(bool connected) {
    obdConnected_ = connected;
}

void SpeedSourceSelector::updateObdSample(float speedMph, uint32_t timestampMs, bool valid) {
    if (!valid || !std::isfinite(speedMph) || speedMph < 0.0f || speedMph > MAX_VALID_SPEED_MPH) {
        return;
    }
    obd_.valid = true;
    obd_.speedMph = speedMph;
    obd_.timestampMs = (timestampMs == 0) ? millis() : timestampMs;
}

void SpeedSourceSelector::updateGpsSample(float speedMph, uint32_t timestampMs, bool valid) {
    if (!valid || !std::isfinite(speedMph) || speedMph < 0.0f || speedMph > MAX_VALID_SPEED_MPH) {
        return;
    }
    gps_.valid = true;
    gps_.speedMph = speedMph;
    gps_.timestampMs = (timestampMs == 0) ? millis() : timestampMs;
}

bool SpeedSourceSelector::select(uint32_t nowMs, SpeedSelection& selection) {
    const bool obdFresh = isSampleFresh(obd_, nowMs, OBD_MAX_AGE_MS);
    const bool gpsFresh = gpsEnabled_ && isSampleFresh(gps_, nowMs, GPS_MAX_AGE_MS);

    SpeedSource nextSource = SpeedSource::NONE;
    const SampleState* nextSample = nullptr;

    if (obdFresh) {
        nextSource = SpeedSource::OBD;
        nextSample = &obd_;
        obdSelections_++;
    } else if (obdConnected_) {
        // When OBD link is active, avoid GPS takeover during transient OBD gaps.
        // This keeps speed-based muting stable and OBD-priority.
        noSourceSelections_++;
    } else if (gpsFresh) {
        nextSource = SpeedSource::GPS;
        nextSample = &gps_;
        gpsSelections_++;
    } else {
        noSourceSelections_++;
    }

    if (nextSource != selectedSource_) {
        sourceSwitches_++;
        selectedSource_ = nextSource;
    }

    selection.source = nextSource;
    if (!nextSample) {
        selection.speedMph = 0.0f;
        selection.timestampMs = 0;
        return false;
    }

    selection.speedMph = nextSample->speedMph;
    selection.timestampMs = nextSample->timestampMs;
    return true;
}

SpeedSelectorStatus SpeedSourceSelector::snapshot(uint32_t nowMs) const {
    SpeedSelectorStatus status;
    status.gpsEnabled = gpsEnabled_;
    status.obdConnected = obdConnected_;
    status.selectedSource = selectedSource_;

    status.obdFresh = isSampleFresh(obd_, nowMs, OBD_MAX_AGE_MS);
    status.obdSpeedMph = obd_.speedMph;
    status.obdAgeMs = sampleAgeMs(obd_, nowMs);

    status.gpsFresh = gpsEnabled_ && isSampleFresh(gps_, nowMs, GPS_MAX_AGE_MS);
    status.gpsSpeedMph = gps_.speedMph;
    status.gpsAgeMs = sampleAgeMs(gps_, nowMs);

    status.sourceSwitches = sourceSwitches_;
    status.obdSelections = obdSelections_;
    status.gpsSelections = gpsSelections_;
    status.noSourceSelections = noSourceSelections_;

    const SampleState* selectedSample = nullptr;
    if (selectedSource_ == SpeedSource::OBD) {
        selectedSample = &obd_;
    } else if (selectedSource_ == SpeedSource::GPS) {
        selectedSample = &gps_;
    }

    if (selectedSample) {
        status.selectedSpeedMph = selectedSample->speedMph;
        status.selectedAgeMs = sampleAgeMs(*selectedSample, nowMs);
    }

    return status;
}

const char* SpeedSourceSelector::sourceName(SpeedSource source) {
    switch (source) {
        case SpeedSource::OBD: return "obd";
        case SpeedSource::GPS: return "gps";
        case SpeedSource::NONE:
        default:
            return "none";
    }
}

bool SpeedSourceSelector::isSampleFresh(const SampleState& sample, uint32_t nowMs, uint32_t maxAgeMs) {
    if (!sample.valid || sample.timestampMs == 0 || nowMs < sample.timestampMs) {
        return false;
    }
    return (nowMs - sample.timestampMs) <= maxAgeMs;
}

uint32_t SpeedSourceSelector::sampleAgeMs(const SampleState& sample, uint32_t nowMs) {
    if (!sample.valid || sample.timestampMs == 0 || nowMs < sample.timestampMs) {
        return UINT32_MAX;
    }
    return nowMs - sample.timestampMs;
}
