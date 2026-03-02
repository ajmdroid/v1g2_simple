#include "speed_source_selector.h"

SpeedSourceSelector speedSourceSelector;

void SpeedSourceSelector::begin(bool gpsEnabled) {
    gpsEnabled_ = gpsEnabled;
}

void SpeedSourceSelector::setGpsEnabled(bool enabled) {
    gpsEnabled_ = enabled;
}

SpeedSelectorStatus SpeedSourceSelector::snapshot(uint32_t nowMs) const {
    (void)nowMs;
    SpeedSelectorStatus status;
    status.gpsEnabled = gpsEnabled_;
    return status;
}

const char* SpeedSourceSelector::sourceName(SpeedSource source) {
    switch (source) {
        case SpeedSource::GPS: return "gps";
        case SpeedSource::NONE:
        default:
            return "none";
    }
}
