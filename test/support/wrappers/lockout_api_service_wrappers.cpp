#include "../../../src/modules/lockout/lockout_api_service.h"

namespace LockoutApiService {

void sendSummary(WebServer& server,
                 SignalObservationLog& signalObservationLog,
                 SignalObservationSdLogger& signalObservationSdLogger);

void sendEvents(WebServer& server,
                SignalObservationLog& signalObservationLog,
                SignalObservationSdLogger& signalObservationSdLogger);

void sendZones(WebServer& server,
               LockoutIndex& lockoutIndex,
               LockoutLearner& lockoutLearner,
               LockoutStore& lockoutStore,
               SettingsManager& settingsManager);

void handleZoneDelete(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

void handleZoneCreate(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

void handleZoneUpdate(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

void sendZoneExport(WebServer& server,
                    LockoutStore& lockoutStore);

void handleZoneImport(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

#ifdef UNIT_TEST
void handleApiSummary(WebServer& server,
                      SignalObservationLog& signalObservationLog,
                      SignalObservationSdLogger& signalObservationSdLogger,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                      void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendSummary(server, signalObservationLog, signalObservationSdLogger);
}
#endif

#ifdef UNIT_TEST
void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendEvents(server, signalObservationLog, signalObservationSdLogger);
}
#endif

#ifdef UNIT_TEST
void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    LockoutStore& lockoutStore,
                    SettingsManager& settingsManager,
                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                    void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendZones(server, lockoutIndex, lockoutLearner, lockoutStore, settingsManager);
}
#endif

#ifdef UNIT_TEST
void handleApiZoneDelete(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleZoneDelete(server, lockoutIndex, lockoutStore);
}
#endif

#ifdef UNIT_TEST
void handleApiZoneCreate(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleZoneCreate(server, lockoutIndex, lockoutStore);
}
#endif

#ifdef UNIT_TEST
void handleApiZoneUpdate(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleZoneUpdate(server, lockoutIndex, lockoutStore);
}
#endif

#ifdef UNIT_TEST
void handleApiZoneExport(WebServer& server,
                         LockoutStore& lockoutStore,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendZoneExport(server, lockoutStore);
}
#endif

#ifdef UNIT_TEST
void handleApiZoneImport(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                         void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleZoneImport(server, lockoutIndex, lockoutStore);
}
#endif

}  // namespace LockoutApiService
