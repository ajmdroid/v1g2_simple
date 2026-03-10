#include "debug_metrics_payload.h"

#include "../../perf_metrics.h"

namespace DebugApiService {

void appendCameraMetricsPayload(JsonDocument& doc) {
    doc["cameraDisplayActive"] = perfCounters.cameraDisplayActive.load();
    doc["cameraDebugOverrideActive"] = perfCounters.cameraDebugOverrideActive.load();
    doc["cameraDisplayFrames"] = perfCounters.cameraDisplayFrames.load();
    doc["cameraDebugDisplayFrames"] = perfCounters.cameraDebugDisplayFrames.load();
    doc["cameraDisplayMaxUs"] = perfExtended.cameraDisplayMaxUs;
    doc["cameraDebugDisplayMaxUs"] = perfExtended.cameraDebugDisplayMaxUs;
    doc["cameraProcessMaxUs"] = perfExtended.cameraProcessMaxUs;
    doc["cameraVoiceQueued"] = perfCounters.cameraVoiceQueued.load();
    doc["cameraVoiceStarted"] = perfCounters.cameraVoiceStarted.load();
}

}  // namespace DebugApiService
