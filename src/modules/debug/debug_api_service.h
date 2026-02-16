#pragma once

#include <Arduino.h>
#include <WebServer.h>

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

}  // namespace DebugApiService
