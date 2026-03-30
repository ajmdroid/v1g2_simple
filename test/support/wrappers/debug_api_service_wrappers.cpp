#include "../../../src/modules/debug/debug_api_service.h"

namespace DebugApiService {

// No-op for unit tests — dependencies are never wired in the native test environment.
void begin(SystemEventBus* /*eventBus*/, V1BLEClient* /*ble*/, BleQueueModule* /*bleQueue*/) {}

#ifdef UNIT_TEST
void sendMetrics(WebServer& server);
void sendPanic(WebServer& server);
void sendV1ScenarioList(WebServer& server);
void sendV1ScenarioStatus(WebServer& server);
void handleDebugEnable(WebServer& server);
void handleMetricsReset(WebServer& server);
void handleV1ScenarioLoad(WebServer& server);
void handleV1ScenarioStart(WebServer& server);
void handleV1ScenarioStop(WebServer& server);
void sendPerfFilesList(WebServer& server);
void handlePerfFileDownload(WebServer& server);
void handlePerfFileDelete(WebServer& server);

void handleApiMetrics(WebServer& server) {
    sendMetrics(server);
}

void handleApiPanic(WebServer& server) {
    sendPanic(server);
}

void handleApiV1ScenarioList(WebServer& server) {
    sendV1ScenarioList(server);
}

void handleApiV1ScenarioStatus(WebServer& server) {
    sendV1ScenarioStatus(server);
}

void handleApiDebugEnable(WebServer& server,
                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleDebugEnable(server);
}

void handleApiMetricsReset(WebServer& server,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleMetricsReset(server);
}

void handleApiV1ScenarioLoad(WebServer& server,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleV1ScenarioLoad(server);
}

void handleApiV1ScenarioStart(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleV1ScenarioStart(server);
}

void handleApiV1ScenarioStop(WebServer& server,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleV1ScenarioStop(server);
}

void handleApiPerfFilesList(WebServer& server,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                            void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendPerfFilesList(server);
}

void handleApiPerfFilesDownload(WebServer& server,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handlePerfFileDownload(server);
}

void handleApiPerfFilesDelete(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                              void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handlePerfFileDelete(server);
}
#endif

}  // namespace DebugApiService
