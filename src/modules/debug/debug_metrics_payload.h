#pragma once

#include <ArduinoJson.h>

namespace DebugApiService {

void appendBleRuntimeMetricsPayload(JsonDocument& doc);
void appendWifiAutoStartMetricsPayload(JsonDocument& doc);

}  // namespace DebugApiService
