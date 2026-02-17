#pragma once

#include <WebServer.h>

#include <functional>

namespace DebugApiService {

/// GET /api/debug/metrics handler for WiFiManager route delegation.
void handleApiMetrics(WebServer& server);

/// GET /api/debug/panic handler for WiFiManager route delegation.
void handleApiPanic(WebServer& server);

/// POST /api/debug/enable handler with route-level rate limiting.
void handleApiDebugEnable(WebServer& server,
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

}  // namespace DebugApiService
