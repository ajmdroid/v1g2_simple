#pragma once

#include <Arduino.h>

enum class SpeedSource : uint8_t {
    NONE = 0,
    GPS = 2,
    OBD = 3
};

struct SpeedSelection {
    SpeedSource source = SpeedSource::NONE;
    float speedMph = 0.0f;
    uint32_t timestampMs = 0;
    uint32_t ageMs = UINT32_MAX;
    bool valid = false;
};

struct SpeedSelectorStatus {
    bool gpsEnabled = false;
    bool obdEnabled = false;
    SpeedSource selectedSource = SpeedSource::NONE;
    float selectedSpeedMph = 0.0f;
    uint32_t selectedAgeMs = UINT32_MAX;

    bool gpsFresh = false;
    float gpsSpeedMph = 0.0f;
    uint32_t gpsAgeMs = UINT32_MAX;

    bool obdFresh = false;
    float obdSpeedMph = 0.0f;
    uint32_t obdAgeMs = UINT32_MAX;

    uint32_t sourceSwitches = 0;
    uint32_t gpsSelections = 0;
    uint32_t obdSelections = 0;
    uint32_t noSourceSelections = 0;
};

class SpeedSourceSelector {
public:
    static constexpr float MAX_VALID_SPEED_MPH = 250.0f;

    void begin(bool gpsEnabled, bool obdEnabled = false);
    void setGpsEnabled(bool enabled);
    void setObdEnabled(bool enabled);
    void update(uint32_t nowMs);

    SpeedSelectorStatus snapshot() const;
    SpeedSelectorStatus snapshot(uint32_t nowMs);
    SpeedSelection selectedSpeed() const { return selectedSpeed_; }

    static const char* sourceName(SpeedSource source);

private:
    SpeedSelectorStatus buildStatus(uint32_t nowMs) const;

    bool gpsEnabled_ = false;
    bool obdEnabled_ = false;

    SpeedSource lastSource_ = SpeedSource::NONE;
    uint32_t sourceSwitches_ = 0;
    uint32_t gpsSelections_ = 0;
    uint32_t obdSelections_ = 0;
    uint32_t noSourceSelections_ = 0;
    SpeedSelectorStatus cachedStatus_ = {};
    SpeedSelection selectedSpeed_ = {};
};

extern SpeedSourceSelector speedSourceSelector;
