#include "lockout_api_service.h"

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
               SettingsManager& settingsManager);

void handleZoneDelete(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

#ifdef UNIT_TEST
void handleApiSummary(WebServer& server,
                      SignalObservationLog& signalObservationLog,
                      SignalObservationSdLogger& signalObservationSdLogger,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity,
                      const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendSummary(server, signalObservationLog, signalObservationSdLogger);
}
#endif

#ifdef UNIT_TEST
void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity,
                     const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendEvents(server, signalObservationLog, signalObservationSdLogger);
}
#endif

void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    SettingsManager& settingsManager,
                    const std::function<bool()>& checkRateLimit,
                    const std::function<void()>& markUiActivity,
                    const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendZones(server, lockoutIndex, lockoutLearner, settingsManager);
}

void handleApiZoneDelete(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity,
                         const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneDelete(server, lockoutIndex, lockoutStore);
}

}  // namespace LockoutApiService
