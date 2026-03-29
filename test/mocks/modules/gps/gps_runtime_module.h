#pragma once

// Use the canonical struct definition — never redefine it here.
// This ensures the mock and real module always agree on the shape of GpsRuntimeStatus.
#include "../../../../src/modules/gps/gps_runtime_status.h"

/**
 * Mock GpsRuntimeModule for native unit testing.
 * Only the class behavior is mocked; GpsRuntimeStatus is the real struct.
 */
class GpsRuntimeModule {
public:
    int snapshotCalls = 0;
    GpsRuntimeStatus nextSnapshot{};

    GpsRuntimeStatus snapshot(uint32_t /*nowMs*/) {
        snapshotCalls++;
        return nextSnapshot;
    }
};
