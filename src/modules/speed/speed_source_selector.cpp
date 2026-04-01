#include "speed_source_selector.h"

#ifndef UNIT_TEST
#include "../obd/obd_runtime_module.h"
#endif

SpeedSourceSelector speedSourceSelector;

void SpeedSourceSelector::wireSpeedSources(ObdRuntimeModule* obd) {
    obd_ = obd;
}

void SpeedSourceSelector::begin(bool obdEnabled) {
    syncEnabledInputs(obdEnabled);
    lastSource_ = SpeedSource::NONE;
    sourceSwitches_ = 0;
    obdSelections_ = 0;
    noSourceSelections_ = 0;
    cachedStatus_ = SpeedSelectorStatus{};
    cachedStatus_.obdEnabled = obdEnabled_;
    selectedSpeed_ = SpeedSelection{};
}

void SpeedSourceSelector::syncEnabledInputs(bool obdEnabled) {
    obdEnabled_ = obdEnabled;
    cachedStatus_.obdEnabled = obdEnabled_;
}

SpeedSelectorStatus SpeedSourceSelector::buildStatus(uint32_t nowMs) const {
    SpeedSelectorStatus status;
    status.obdEnabled = obdEnabled_;

    float obdSpeed = 0.0f;
    uint32_t obdTs = 0;
    if (obdEnabled_ && obd_ && obd_->getFreshSpeed(nowMs, obdSpeed, obdTs) &&
        obdSpeed <= MAX_VALID_SPEED_MPH) {
        status.obdFresh = true;
        status.obdSpeedMph = obdSpeed;
        status.obdAgeMs = nowMs - obdTs;
    }

    if (status.obdFresh) {
        status.selectedSource = SpeedSource::OBD;
        status.selectedSpeedMph = status.obdSpeedMph;
        status.selectedAgeMs = status.obdAgeMs;
    }

    status.sourceSwitches = sourceSwitches_;
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
    } else {
        noSourceSelections_++;
        selectedSpeed_ = SpeedSelection{};
    }

    if (picked != lastSource_ && lastSource_ != SpeedSource::NONE) {
        sourceSwitches_++;
    }
    lastSource_ = picked;

    next.sourceSwitches = sourceSwitches_;
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
        case SpeedSource::OBD: return "obd";
        case SpeedSource::NONE:
        default:
            return "none";
    }
}
