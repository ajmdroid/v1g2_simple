#include "../../../src/modules/debug/debug_api_service.h"

namespace DebugApiService {

#ifdef UNIT_TEST
void sendMetrics(WebServer& server);
void sendPanic(WebServer& server);
void handleDebugEnable(WebServer& server);
void handleMetricsReset(WebServer& server);
void sendPerfFilesList(WebServer& server);
void handlePerfFileDownload(WebServer& server);
void handlePerfFileDelete(WebServer& server);

void handleApiMetrics(WebServer& server) {
    sendMetrics(server);
}

void handleApiPanic(WebServer& server) {
    sendPanic(server);
}

void handleApiDebugEnable(WebServer& server,
                          const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleDebugEnable(server);
}

void handleApiMetricsReset(WebServer& server,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleMetricsReset(server);
}

void handleApiPerfFilesList(WebServer& server,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendPerfFilesList(server);
}

void handleApiPerfFilesDownload(WebServer& server,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDownload(server);
}

void handleApiPerfFilesDelete(WebServer& server,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDelete(server);
}
#endif

}  // namespace DebugApiService
