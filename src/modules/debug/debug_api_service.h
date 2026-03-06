#pragma once

#include <WebServer.h>

#include <functional>

namespace DebugApiService {

/// GET /api/debug/metrics handler for WiFiManager route delegation.
void handleApiMetrics(WebServer& server);

/// GET /api/debug/panic handler for WiFiManager route delegation.
void handleApiPanic(WebServer& server);

/// GET /api/debug/v1-scenario/list handler.
void handleApiV1ScenarioList(WebServer& server);

/// GET /api/debug/v1-scenario/status handler.
void handleApiV1ScenarioStatus(WebServer& server);

/// POST /api/debug/enable handler with route-level rate limiting.
void handleApiDebugEnable(WebServer& server,
                          const std::function<bool()>& checkRateLimit);

/// POST /api/debug/metrics/reset handler with route-level rate limiting.
void handleApiMetricsReset(WebServer& server,
                           const std::function<bool()>& checkRateLimit);

/// POST /api/debug/proxy-advertising handler with route-level rate limiting.
void handleApiProxyAdvertisingControl(WebServer& server,
                                      const std::function<bool()>& checkRateLimit);

/// POST /api/debug/camera-alert/render handler with route-level rate limiting.
void handleApiCameraAlertRender(WebServer& server,
                                const std::function<bool()>& checkRateLimit);

/// POST /api/debug/camera-alert/clear handler with route-level rate limiting.
void handleApiCameraAlertClear(WebServer& server,
                               const std::function<bool()>& checkRateLimit);

/// POST /api/debug/v1-scenario/load handler with route-level rate limiting.
void handleApiV1ScenarioLoad(WebServer& server,
                             const std::function<bool()>& checkRateLimit);

/// POST /api/debug/v1-scenario/start handler with route-level rate limiting.
void handleApiV1ScenarioStart(WebServer& server,
                              const std::function<bool()>& checkRateLimit);

/// POST /api/debug/v1-scenario/stop handler with route-level rate limiting.
void handleApiV1ScenarioStop(WebServer& server,
                             const std::function<bool()>& checkRateLimit);

/// GET /api/debug/perf-files handler with route-level policy callbacks.
void handleApiPerfFilesList(WebServer& server,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity);

/// GET /api/debug/perf-files/download handler with route-level policy callbacks.
void handleApiPerfFilesDownload(WebServer& server,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity);

/// POST /api/debug/perf-files/delete handler with route-level policy callbacks.
void handleApiPerfFilesDelete(WebServer& server,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& markUiActivity);

/// Per-loop debug processing (scenario packet injection).
void process(uint32_t nowMs);

}  // namespace DebugApiService
