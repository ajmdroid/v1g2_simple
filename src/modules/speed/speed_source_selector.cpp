#include "speed_source_selector.h"

#ifndef UNIT_TEST
#include "../gps/gps_runtime_module.h"
#include "../obd/obd_runtime_module.h"
#endif

SpeedSourceSelector speedSourceSelector;

void SpeedSourceSelector::begin(bool gpsEnabled, bool obdEnabled) {
    syncEnabledInputs(gpsEnabled, obdEnabled);
    lastSource_ = SpeedSource::NONE;
    sourceSwitches_ = 0;
    gpsSelections_ = 0;
    obdSelections_ = 0;
    noSourceSelections_ = 0;
    cachedStatus_ = SpeedSelectorStatus{};
    cachedStatus_.gpsEnabled = gpsEnabled_;
    cachedStatus_.obdEnabled = obdEnabled_;
    selectedSpeed_ = SpeedSelection{};
}

void SpeedSourceSelector::syncEnabledInputs(bool gpsEnabled, bool obdEnabled) {
    gpsEnabled_ = gpsEnabled;
    obdEnabled_ = obdEnabled;
    cachedStatus_.gpsEnabled = gpsEnabled_;
    cachedStatus_.obdEnabled = obdEnabled_;
}

SpeedSelectorStatus SpeedSourceSelector::buildStatus(uint32_t nowMs) const {
    SpeedSelectorStatus status;
    status.gpsEnabled = gpsEnabled_;
    status.obdEnabled = obdEnabled_;

    float gpsSpeed = 0.0f;
    uint32_t gpsTs = 0;
    if (gpsEnabled_ && gpsRuntimeModule.getFreshSpeed(nowMs, gpsSpeed, gpsTs) &&
        gpsSpeed <= MAX_VALID_SPEED_MPH) {
        status.gpsFresh = true;
        status.gpsSpeedMph = gpsSpeed;
        status.gpsAgeMs = nowMs - gpsTs;
    }

    float obdSpeed = 0.0f;
    uint32_t obdTs = 0;
    if (obdEnabled_ && obdRuntimeModule.getFreshSpeed(nowMs, obdSpeed, obdTs) &&
        obdSpeed <= MAX_VALID_SPEED_MPH) {
        status.obdFresh = true;
        status.obdSpeedMph = obdSpeed;
        status.obdAgeMs = nowMs - obdTs;
    }

    if (status.obdFresh) {
        status.selectedSource = SpeedSource::OBD;
        status.selectedSpeedMph = status.obdSpeedMph;
        status.selectedAgeMs = status.obdAgeMs;
    } else if (status.gpsFresh) {
        status.selectedSource = SpeedSource::GPS;
        status.selectedSpeedMph = status.gpsSpeedMph;
        status.selectedAgeMs = status.gpsAgeMs;
    }

    status.sourceSwitches = sourceSwitches_;
    status.gpsSelections = gpsSelections_;
    status.obdSelections = obdSelections_;
    status.noSourceSelections = noSourceSelections_;
    return status;
}

void SpeedSourceSelector::update(uint32_t nowMs) {
    SpeedSelectorStatus next = buildStatus(nowMs);
    const SpeedSource picked = next.selectedSource;

    if (picked == SpeedSource::OBD) {
        obdSelections_++;
        selectedSpeed_.source = SpeedSource::OBD;
        selectedSpeed_.speedMph = next.selectedSpeedMph;
        selectedSpeed_.timestampMs = nowMs - next.selectedAgeMs;
        selectedSpeed_.ageMs = next.selectedAgeMs;
        selectedSpeed_.valid = true;
    } else if (picked == SpeedSource::GPS) {
        gpsSelections_++;
        selectedSpeed_.source = SpeedSource::GPS;
        selectedSpeed_.speedMph = next.selectedSpeedMph;
        selectedSpeed_.timestampMs = nowMs - next.selectedAgeMs;
        selectedSpeed_.ageMs = next.selectedAgeMs;
        selectedSpeed_.valid = true;
    } else {
        noSourceSelections_++;
        selectedSpeed_ = SpeedSelection{};
    }

    if (picked != lastSource_ && lastSource_ != SpeedSource::NONE) {
        sourceSwitches_++;
    }
    lastSource_ = picked;

    next.sourceSwitches = sourceSwitches_;
    next.gpsSelections = gpsSelections_;
    next.obdSelections = obdSelections_;
    next.noSourceSelections = noSourceSelections_;
    cachedStatus_ = next;
}

SpeedSelectorStatus SpeedSourceSelector::snapshot() const {
    return cachedStatus_;
}

SpeedSelectorStatus SpeedSourceSelector::snapshotAt(uint32_t nowMs) const {
    return buildStatus(nowMs);
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
