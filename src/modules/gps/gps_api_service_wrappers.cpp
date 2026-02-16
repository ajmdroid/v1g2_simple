#include "gps_api_service.h"

namespace GpsApiService {

void sendStatus(WebServer& server,
                GpsRuntimeModule& gpsRuntimeModule,
                SpeedSourceSelector& speedSourceSelector,
                SettingsManager& settingsManager,
                GpsObservationLog& gpsObservationLog,
                LockoutLearner& lockoutLearner,
                PerfCounters& perfCounters,
                SystemEventBus& systemEventBus);

void sendObservations(WebServer& server,
                      GpsObservationLog& gpsObservationLog);

void handleConfig(WebServer& server,
                  SettingsManager& settingsManager,
                  GpsRuntimeModule& gpsRuntimeModule,
                  SpeedSourceSelector& speedSourceSelector,
                  LockoutLearner& lockoutLearner,
                  GpsObservationLog& gpsObservationLog,
                  PerfCounters& perfCounters,
                  SystemEventBus& systemEventBus);

void handleApiStatus(WebServer& server,
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

void handleApiObservations(WebServer& server,
                           GpsObservationLog& gpsObservationLog,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendObservations(server, gpsObservationLog);
}

void handleApiConfig(WebServer& server,
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
