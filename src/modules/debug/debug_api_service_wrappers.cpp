#include "debug_api_service.h"

namespace DebugApiService {

#ifdef UNIT_TEST
void sendMetrics(WebServer& server);
void sendPanic(WebServer& server);

void handleApiMetrics(WebServer& server) {
    sendMetrics(server);
}

void handleApiPanic(WebServer& server) {
    sendPanic(server);
}
#endif

void handleDebugEnable(WebServer& server);
void sendPerfFilesList(WebServer& server);
void handlePerfFileDownload(WebServer& server);
void handlePerfFileDelete(WebServer& server);

void handleApiDebugEnable(WebServer& server,
                          const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleDebugEnable(server);
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

}  // namespace DebugApiService
