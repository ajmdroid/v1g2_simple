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

/// GET /api/lockouts/summary handler with route-level policy callbacks.
void handleApiSummary(WebServer& server,
                      SignalObservationLog& signalObservationLog,
                      SignalObservationSdLogger& signalObservationSdLogger,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

/// GET /api/lockouts/events handler with route-level policy callbacks.
void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

/// GET /api/lockouts/zones handler with route-level policy callbacks.
void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    LockoutStore& lockoutStore,
                    SettingsManager& settingsManager,
                    const std::function<bool()>& checkRateLimit,
                    const std::function<void()>& markUiActivity);

/// POST /api/lockouts/zones/delete handler with route-level policy callbacks.
void handleApiZoneDelete(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

/// POST /api/lockouts/zones/create handler with route-level policy callbacks.
void handleApiZoneCreate(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

/// POST /api/lockouts/zones/update handler with route-level policy callbacks.
void handleApiZoneUpdate(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

/// GET /api/lockouts/zones/export handler with route-level policy callbacks.
void handleApiZoneExport(WebServer& server,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

/// POST /api/lockouts/zones/import handler with route-level policy callbacks.
void handleApiZoneImport(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

/// POST /api/lockouts/pending/clear handler with route-level policy callbacks.
void handleApiPendingClear(WebServer& server,
                           LockoutLearner& lockoutLearner,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity);

}  // namespace LockoutApiService
