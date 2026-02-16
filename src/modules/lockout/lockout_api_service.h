#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

class LockoutIndex;
class LockoutStore;
class LockoutLearner;
class SignalObservationLog;
class SignalObservationSdLogger;
class SettingsManager;

namespace LockoutApiService {

/// GET /api/lockout/summary — observation stats + latest sample.
void sendSummary(WebServer& server,
                 SignalObservationLog& signalObservationLog,
                 SignalObservationSdLogger& signalObservationSdLogger);

/// GET /api/lockout/events — recent signal observations.
void sendEvents(WebServer& server,
                SignalObservationLog& signalObservationLog,
                SignalObservationSdLogger& signalObservationSdLogger);

/// GET /api/lockout/zones — active + pending lockout zones.
void sendZones(WebServer& server,
               LockoutIndex& lockoutIndex,
               LockoutLearner& lockoutLearner,
               SettingsManager& settingsManager);

/// DELETE /api/lockout/zones — delete a learned lockout zone by slot.
void handleZoneDelete(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

/// GET /api/lockout/summary wrapper with route-level policy callbacks.
inline void handleApiSummary(WebServer& server,
                             SignalObservationLog& signalObservationLog,
                             SignalObservationSdLogger& signalObservationSdLogger,
                             const std::function<bool()>& checkRateLimit,
                             const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendSummary(server, signalObservationLog, signalObservationSdLogger);
}

/// GET /api/lockout/events wrapper with route-level policy callbacks.
inline void handleApiEvents(WebServer& server,
                            SignalObservationLog& signalObservationLog,
                            SignalObservationSdLogger& signalObservationSdLogger,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendEvents(server, signalObservationLog, signalObservationSdLogger);
}

/// GET /api/lockout/zones wrapper with route-level policy callbacks.
inline void handleApiZones(WebServer& server,
                           LockoutIndex& lockoutIndex,
                           LockoutLearner& lockoutLearner,
                           SettingsManager& settingsManager,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendZones(server, lockoutIndex, lockoutLearner, settingsManager);
}

/// POST /api/lockout/zones/delete wrapper with route-level policy callbacks.
inline void handleApiZoneDelete(WebServer& server,
                                LockoutIndex& lockoutIndex,
                                LockoutStore& lockoutStore,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneDelete(server, lockoutIndex, lockoutStore);
}

}  // namespace LockoutApiService
