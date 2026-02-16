#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

namespace DebugApiService {

/// GET /api/debug/metrics — full diagnostic snapshot.
void sendMetrics(WebServer& server);

/// POST /api/debug/enable — enable/disable perf debug logging.
void handleDebugEnable(WebServer& server);

/// GET /api/debug/panic — last panic/reset info.
void sendPanic(WebServer& server);

/// GET /api/debug/perf/files — list perf CSV files on SD.
void sendPerfFilesList(WebServer& server);

/// GET /api/debug/perf/download — download a perf CSV file.
void handlePerfFileDownload(WebServer& server);

/// DELETE /api/debug/perf/delete — delete a perf CSV file.
void handlePerfFileDelete(WebServer& server);

/// GET /api/debug/metrics wrapper for WiFiManager route delegation.
inline void handleApiMetrics(WebServer& server) {
    sendMetrics(server);
}

/// GET /api/debug/panic wrapper for WiFiManager route delegation.
inline void handleApiPanic(WebServer& server) {
    sendPanic(server);
}

/// POST /api/debug/enable wrapper with route-level rate limiting.
inline void handleApiDebugEnable(WebServer& server,
                                 const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleDebugEnable(server);
}

/// GET /api/debug/perf-files wrapper with route-level policy callbacks.
inline void handleApiPerfFilesList(WebServer& server,
                                   const std::function<bool()>& checkRateLimit,
                                   const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendPerfFilesList(server);
}

/// GET /api/debug/perf-files/download wrapper with route-level policy callbacks.
inline void handleApiPerfFilesDownload(WebServer& server,
                                       const std::function<bool()>& checkRateLimit,
                                       const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDownload(server);
}

/// POST /api/debug/perf-files/delete wrapper with route-level policy callbacks.
inline void handleApiPerfFilesDelete(WebServer& server,
                                     const std::function<bool()>& checkRateLimit,
                                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDelete(server);
}

}  // namespace DebugApiService
