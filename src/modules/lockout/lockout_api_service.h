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
void handleApiSummary(WebServer& server,
                      SignalObservationLog& signalObservationLog,
                      SignalObservationSdLogger& signalObservationSdLogger,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity,
                      const std::function<void()>& sendDeprecatedHeader = {});

/// GET /api/lockout/events wrapper with route-level policy callbacks.
void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity,
                     const std::function<void()>& sendDeprecatedHeader = {});

/// GET /api/lockout/zones wrapper with route-level policy callbacks.
void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    SettingsManager& settingsManager,
                    const std::function<bool()>& checkRateLimit,
                    const std::function<void()>& markUiActivity,
                    const std::function<void()>& sendDeprecatedHeader = {});

/// POST /api/lockout/zones/delete wrapper with route-level policy callbacks.
void handleApiZoneDelete(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity,
                         const std::function<void()>& sendDeprecatedHeader = {});

}  // namespace LockoutApiService
