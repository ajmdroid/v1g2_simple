/**
 * WiFi Time Sync API service.
 *
 * Provides POST /api/time/sync — sets device epoch time from the browser clock.
 * Called automatically by the web UI on first page load so the perf CSV logger
 * gets a timestamped filename even without GPS or SNTP.
 */

#pragma once

#include <WebServer.h>
#include <cstdint>

namespace WifiTimeApiService {

/// POST /api/time/sync
/// Body (JSON): { "epochMs": <int64>, "tzOffsetMinutes": <int32, optional> }
/// Response 200: { "ok": true, "source": 1, "epochMs": <int64> }
/// Response 400: { "ok": false, "error": "...", "message": "..." }
///
/// The setTime callback receives the validated epoch and timezone and is
/// responsible for calling timeService.setEpochBaseMs(). Injected for
/// testability — the route lambda in wifi_routes.cpp wires the real service.
void handleApiTimeSync(WebServer& server,
                       bool (*setTime)(int64_t epochMs, int32_t tzOffsetMinutes, void* ctx),
                       void* setTimeCtx);

}  // namespace WifiTimeApiService
