#pragma once

#include <WebServer.h>

// Forward declarations for begin() dependencies.
class SystemEventBus;
class V1BLEClient;
class BleQueueModule;

namespace DebugApiService {

/// Wire dependencies. Must be called once during setup() before any handlers
/// or process() are invoked. All pointers must remain valid for the lifetime
/// of the service.
void begin(SystemEventBus* eventBus, V1BLEClient* ble, BleQueueModule* bleQueue);


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
                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/debug/metrics/reset handler with route-level rate limiting.
void handleApiMetricsReset(WebServer& server,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/debug/proxy-advertising handler with route-level rate limiting.
void handleApiProxyAdvertisingControl(WebServer& server,
                                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/debug/v1-scenario/load handler with route-level rate limiting.
void handleApiV1ScenarioLoad(WebServer& server,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/debug/v1-scenario/start handler with route-level rate limiting.
void handleApiV1ScenarioStart(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// POST /api/debug/v1-scenario/stop handler with route-level rate limiting.
void handleApiV1ScenarioStop(WebServer& server,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

/// GET /api/debug/perf-files handler with route-level policy callbacks.
void handleApiPerfFilesList(WebServer& server,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                            void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// GET /api/debug/perf-files/download handler with route-level policy callbacks.
void handleApiPerfFilesDownload(WebServer& server,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// POST /api/debug/perf-files/delete handler with route-level policy callbacks.
void handleApiPerfFilesDelete(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                              void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// Per-loop debug processing (scenario packet injection).
void process(uint32_t nowMs);

}  // namespace DebugApiService
