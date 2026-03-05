#pragma once

#include <cstdint>

struct GpsRuntimeStatus {
    bool enabled = false;
    bool sampleValid = false;
    bool hasFix = false;
    bool stableHasFix = false;
    uint8_t satellites = 0;
    uint8_t stableSatellites = 0;
    float hdop = 0.0f;
    bool locationValid = false;
    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    bool courseValid = false;
    float courseDeg = 0.0f;
    uint32_t courseSampleTsMs = 0;
    uint32_t courseAgeMs = 0;
    uint32_t sampleTsMs = 0;
    uint32_t sampleAgeMs = 0;
    uint32_t fixAgeMs = 0;
    uint32_t stableFixAgeMs = 0;
    uint32_t injectedSamples = 0;
    bool moduleDetected = false;
    bool detectionTimedOut = false;
    bool parserActive = false;
    uint32_t hardwareSamples = 0;
    uint32_t bytesRead = 0;
    uint32_t sentencesSeen = 0;
    uint32_t sentencesParsed = 0;
    uint32_t parseFailures = 0;
    uint32_t checksumFailures = 0;
    uint32_t bufferOverruns = 0;
    uint32_t lastSentenceTsMs = 0;
};

class GpsRuntimeModule {
public:
    int snapshotCalls = 0;
    GpsRuntimeStatus nextSnapshot{};

    GpsRuntimeStatus snapshot(uint32_t /*nowMs*/) {
        snapshotCalls++;
        return nextSnapshot;
    }
};
