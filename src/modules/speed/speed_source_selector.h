#pragma once

#include <Arduino.h>

enum class SpeedSource : uint8_t {
    NONE = 0,
    GPS = 2
};

struct SpeedSelection {
    SpeedSource source = SpeedSource::NONE;
    float speedMph = 0.0f;
    uint32_t timestampMs = 0;
};

struct SpeedSelectorStatus {
    bool gpsEnabled = false;
    SpeedSource selectedSource = SpeedSource::NONE;
    float selectedSpeedMph = 0.0f;
    uint32_t selectedAgeMs = UINT32_MAX;

    bool gpsFresh = false;
    float gpsSpeedMph = 0.0f;
    uint32_t gpsAgeMs = UINT32_MAX;

    uint32_t sourceSwitches = 0;
    uint32_t gpsSelections = 0;
    uint32_t noSourceSelections = 0;
};

class SpeedSourceSelector {
public:
    static constexpr float MAX_VALID_SPEED_MPH = 250.0f;

    void begin(bool gpsEnabled);
    void setGpsEnabled(bool enabled);

    bool select(uint32_t nowMs, SpeedSelection& selection);
    SpeedSelectorStatus snapshot(uint32_t nowMs) const;

    static const char* sourceName(SpeedSource source);

private:
    bool gpsEnabled_ = false;
};

extern SpeedSourceSelector speedSourceSelector;
