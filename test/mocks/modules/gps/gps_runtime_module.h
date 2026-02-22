#pragma once

#include <cstdint>

struct GpsRuntimeStatus {
    bool enabled = false;
    bool hasFix = false;
    bool stableHasFix = false;
    uint8_t satellites = 0;
    uint8_t stableSatellites = 0;
    bool locationValid = false;
    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
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
