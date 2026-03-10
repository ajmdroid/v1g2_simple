#pragma once

#include <ArduinoJson.h>

namespace DebugApiService {

// Appends the camera attribution fields shared by normal and soak metrics.
void appendCameraMetricsPayload(JsonDocument& doc);

}  // namespace DebugApiService
