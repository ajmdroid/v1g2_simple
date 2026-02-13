#pragma once

#include <Arduino.h>

struct GpsRuntimeStatus {
    bool enabled = false;
    bool sampleValid = false;
    bool hasFix = false;
    float speedMph = 0.0f;
    uint8_t satellites = 0;
    float hdop = NAN;
    uint32_t sampleTsMs = 0;
    uint32_t sampleAgeMs = UINT32_MAX;
    uint32_t injectedSamples = 0;
};

class GpsRuntimeModule {
public:
    // Freshness budget for speed fallback use.
    static constexpr uint32_t SAMPLE_MAX_AGE_MS = 3000;

    void begin(bool enabled);
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // Placeholder for future non-blocking hardware parser work.
    void update(uint32_t nowMs);

    // Stage-2 scaffold path used by API/tools until hardware driver lands.
    void setScaffoldSample(float speedMph,
                           bool hasFix,
                           uint8_t satellites,
                           float hdop,
                           uint32_t timestampMs);
    void clearSample();

    bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const;
    GpsRuntimeStatus snapshot(uint32_t nowMs) const;

private:
    bool enabled_ = false;
    bool sampleValid_ = false;
    bool hasFix_ = false;
    float speedMph_ = 0.0f;
    uint8_t satellites_ = 0;
    float hdop_ = NAN;
    uint32_t sampleTsMs_ = 0;
    uint32_t injectedSamples_ = 0;
};

extern GpsRuntimeModule gpsRuntimeModule;
