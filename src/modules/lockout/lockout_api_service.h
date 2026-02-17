#pragma once

#include <WebServer.h>

#include <functional>

class LockoutIndex;
class LockoutStore;
class LockoutLearner;
class SignalObservationLog;
class SignalObservationSdLogger;
class SettingsManager;

namespace LockoutApiService {

/// GET /api/lockout/summary handler with route-level policy callbacks.
void handleApiSummary(WebServer& server,
                      SignalObservationLog& signalObservationLog,
                      SignalObservationSdLogger& signalObservationSdLogger,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity,
                      const std::function<void()>& sendDeprecatedHeader = {});

/// GET /api/lockout/events handler with route-level policy callbacks.
void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity,
                     const std::function<void()>& sendDeprecatedHeader = {});

/// GET /api/lockout/zones handler with route-level policy callbacks.
void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    SettingsManager& settingsManager,
                    const std::function<bool()>& checkRateLimit,
                    const std::function<void()>& markUiActivity,
                    const std::function<void()>& sendDeprecatedHeader = {});

/// POST /api/lockout/zones/delete handler with route-level policy callbacks.
void handleApiZoneDelete(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity,
                         const std::function<void()>& sendDeprecatedHeader = {});

}  // namespace LockoutApiService
