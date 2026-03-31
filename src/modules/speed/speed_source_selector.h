#pragma once
#ifndef SPEED_SOURCE_SELECTOR_H
#define SPEED_SOURCE_SELECTOR_H

#include <Arduino.h>

class GpsRuntimeModule;
class ObdRuntimeModule;

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
    void wireSpeedSources(GpsRuntimeModule* gps, ObdRuntimeModule* obd);
    void syncEnabledInputs(bool gpsEnabled, bool obdEnabled);
    void update(uint32_t nowMs);

    // Producers call update(nowMs) once per loop to commit state.
    // Consumers read snapshot() for the committed state or snapshotAt(nowMs)
    // for a pure point-in-time view that does not mutate counters/state.
    SpeedSelectorStatus snapshot() const;
    SpeedSelectorStatus snapshotAt(uint32_t nowMs) const;
    SpeedSelection selectedSpeed() const { return selectedSpeed_; }

    static const char* sourceName(SpeedSource source);

private:
    SpeedSelectorStatus buildStatus(uint32_t nowMs) const;

    bool gpsEnabled_ = false;
    bool obdEnabled_ = false;

    GpsRuntimeModule* gps_ = nullptr;
    ObdRuntimeModule* obd_ = nullptr;

    SpeedSource lastSource_ = SpeedSource::NONE;
    uint32_t sourceSwitches_ = 0;
    uint32_t gpsSelections_ = 0;
    uint32_t obdSelections_ = 0;
    uint32_t noSourceSelections_ = 0;
    SpeedSelectorStatus cachedStatus_ = {};
    SpeedSelection selectedSpeed_ = {};
};
#endif // SPEED_SOURCE_SELECTOR_H
