#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

class GpsRuntimeModule;
class GpsObservationLog;
class SpeedSourceSelector;
class LockoutLearner;
class SettingsManager;
struct PerfCounters;
class SystemEventBus;

namespace GpsApiService {

/// GET /api/gps/status — full GPS + lockout snapshot.
void sendStatus(WebServer& server,
                GpsRuntimeModule& gpsRuntimeModule,
                SpeedSourceSelector& speedSourceSelector,
                SettingsManager& settingsManager,
                GpsObservationLog& gpsObservationLog,
                LockoutLearner& lockoutLearner,
                PerfCounters& perfCounters,
                SystemEventBus& systemEventBus);

/// GET /api/gps/observations — recent observation ring-buffer dump.
void sendObservations(WebServer& server,
                      GpsObservationLog& gpsObservationLog);

/// POST /api/gps/config — enable/disable GPS, scaffold inject, lockout tuning.
void handleConfig(WebServer& server,
                  SettingsManager& settingsManager,
                  GpsRuntimeModule& gpsRuntimeModule,
                  SpeedSourceSelector& speedSourceSelector,
                  LockoutLearner& lockoutLearner,
                  GpsObservationLog& gpsObservationLog,
                  PerfCounters& perfCounters,
                  SystemEventBus& systemEventBus);

/// GET /api/gps/status wrapper for WiFiManager route delegation.
inline void handleApiStatus(WebServer& server,
                            GpsRuntimeModule& gpsRuntimeModule,
                            SpeedSourceSelector& speedSourceSelector,
                            SettingsManager& settingsManager,
                            GpsObservationLog& gpsObservationLog,
                            LockoutLearner& lockoutLearner,
                            PerfCounters& perfCounters,
                            SystemEventBus& systemEventBus,
                            const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendStatus(server,
               gpsRuntimeModule,
               speedSourceSelector,
               settingsManager,
               gpsObservationLog,
               lockoutLearner,
               perfCounters,
               systemEventBus);
}

/// GET /api/gps/observations wrapper with route-level policy callbacks.
inline void handleApiObservations(WebServer& server,
                                  GpsObservationLog& gpsObservationLog,
                                  const std::function<bool()>& checkRateLimit,
                                  const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendObservations(server, gpsObservationLog);
}

/// POST /api/gps/config wrapper with route-level policy callbacks.
inline void handleApiConfig(WebServer& server,
                            SettingsManager& settingsManager,
                            GpsRuntimeModule& gpsRuntimeModule,
                            SpeedSourceSelector& speedSourceSelector,
                            LockoutLearner& lockoutLearner,
                            GpsObservationLog& gpsObservationLog,
                            PerfCounters& perfCounters,
                            SystemEventBus& systemEventBus,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleConfig(server,
                 settingsManager,
                 gpsRuntimeModule,
                 speedSourceSelector,
                 lockoutLearner,
                 gpsObservationLog,
                 perfCounters,
                 systemEventBus);
}

}  // namespace GpsApiService
