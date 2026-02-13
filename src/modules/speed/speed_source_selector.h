#pragma once

#include <Arduino.h>

enum class SpeedSource : uint8_t {
    NONE = 0,
    OBD = 1,
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

    bool obdFresh = false;
    float obdSpeedMph = 0.0f;
    uint32_t obdAgeMs = UINT32_MAX;

    bool gpsFresh = false;
    float gpsSpeedMph = 0.0f;
    uint32_t gpsAgeMs = UINT32_MAX;

    uint32_t sourceSwitches = 0;
    uint32_t obdSelections = 0;
    uint32_t gpsSelections = 0;
    uint32_t noSourceSelections = 0;
};

class SpeedSourceSelector {
public:
    static constexpr uint32_t OBD_MAX_AGE_MS = 3000;
    static constexpr uint32_t GPS_MAX_AGE_MS = 3000;
    static constexpr float MAX_VALID_SPEED_MPH = 250.0f;

    void begin(bool gpsEnabled);
    void setGpsEnabled(bool enabled);
    bool isGpsEnabled() const { return gpsEnabled_; }

    void updateObdSample(float speedMph, uint32_t timestampMs, bool valid);
    void updateGpsSample(float speedMph, uint32_t timestampMs, bool valid);

    bool select(uint32_t nowMs, SpeedSelection& selection);
    SpeedSelectorStatus snapshot(uint32_t nowMs) const;

    static const char* sourceName(SpeedSource source);

private:
    struct SampleState {
        bool valid = false;
        float speedMph = 0.0f;
        uint32_t timestampMs = 0;
    };

    static bool isSampleFresh(const SampleState& sample, uint32_t nowMs, uint32_t maxAgeMs);
    static uint32_t sampleAgeMs(const SampleState& sample, uint32_t nowMs);

    bool gpsEnabled_ = false;
    SampleState obd_;
    SampleState gps_;

    SpeedSource selectedSource_ = SpeedSource::NONE;
    uint32_t sourceSwitches_ = 0;
    uint32_t obdSelections_ = 0;
    uint32_t gpsSelections_ = 0;
    uint32_t noSourceSelections_ = 0;
};

extern SpeedSourceSelector speedSourceSelector;
