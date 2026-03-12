#include "speed_source_selector.h"

#ifndef UNIT_TEST
#include "../gps/gps_runtime_module.h"
#include "../obd/obd_runtime_module.h"
#endif

SpeedSourceSelector speedSourceSelector;

void SpeedSourceSelector::begin(bool gpsEnabled, bool obdEnabled) {
    gpsEnabled_ = gpsEnabled;
    obdEnabled_ = obdEnabled;
    lastSource_ = SpeedSource::NONE;
    sourceSwitches_ = 0;
    gpsSelections_ = 0;
    obdSelections_ = 0;
    noSourceSelections_ = 0;
}

void SpeedSourceSelector::setGpsEnabled(bool enabled) {
    gpsEnabled_ = enabled;
}

void SpeedSourceSelector::setObdEnabled(bool enabled) {
    obdEnabled_ = enabled;
}

SpeedSelectorStatus SpeedSourceSelector::snapshot(uint32_t nowMs) const {
    SpeedSelectorStatus status;
    status.gpsEnabled = gpsEnabled_;
    status.obdEnabled = obdEnabled_;

    // Poll GPS
    float gpsSpeed = 0.0f;
    uint32_t gpsTs = 0;
    bool gpsFresh = false;
    if (gpsEnabled_) {
        gpsFresh = gpsRuntimeModule.getFreshSpeed(nowMs, gpsSpeed, gpsTs);
        if (gpsFresh && gpsSpeed <= MAX_VALID_SPEED_MPH) {
            status.gpsFresh = true;
            status.gpsSpeedMph = gpsSpeed;
            status.gpsAgeMs = nowMs - gpsTs;
        }
    }

    // Poll OBD
    float obdSpeed = 0.0f;
    uint32_t obdTs = 0;
    bool obdFresh = false;
    if (obdEnabled_) {
        obdFresh = obdRuntimeModule.getFreshSpeed(nowMs, obdSpeed, obdTs);
        if (obdFresh && obdSpeed <= MAX_VALID_SPEED_MPH) {
            status.obdFresh = true;
            status.obdSpeedMph = obdSpeed;
            status.obdAgeMs = nowMs - obdTs;
        }
    }

    // Select best source: prefer OBD when both fresh (faster update rate)
    SpeedSource picked = SpeedSource::NONE;
    if (status.obdFresh && status.gpsFresh) {
        picked = SpeedSource::OBD;  // OBD preferred: 2 Hz vs GPS 1 Hz
    } else if (status.obdFresh) {
        picked = SpeedSource::OBD;
    } else if (status.gpsFresh) {
        picked = SpeedSource::GPS;
    }

    if (picked == SpeedSource::OBD) {
        status.selectedSource = SpeedSource::OBD;
        status.selectedSpeedMph = status.obdSpeedMph;
        status.selectedAgeMs = status.obdAgeMs;
        obdSelections_++;
    } else if (picked == SpeedSource::GPS) {
        status.selectedSource = SpeedSource::GPS;
        status.selectedSpeedMph = status.gpsSpeedMph;
        status.selectedAgeMs = status.gpsAgeMs;
        gpsSelections_++;
    } else {
        noSourceSelections_++;
    }

    if (picked != lastSource_ && lastSource_ != SpeedSource::NONE) {
        sourceSwitches_++;
    }
    lastSource_ = picked;

    status.sourceSwitches = sourceSwitches_;
    status.gpsSelections = gpsSelections_;
    status.obdSelections = obdSelections_;
    status.noSourceSelections = noSourceSelections_;

    return status;
}

const char* SpeedSourceSelector::sourceName(SpeedSource source) {
    switch (source) {
        case SpeedSource::GPS: return "gps";
        case SpeedSource::OBD: return "obd";
        case SpeedSource::NONE:
        default:
            return "none";
    }
}
