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

/// GET /api/gps/status wrapper for WiFiManager route delegation.
void handleApiStatus(WebServer& server,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     SettingsManager& settingsManager,
                     GpsObservationLog& gpsObservationLog,
                     LockoutLearner& lockoutLearner,
                     PerfCounters& perfCounters,
                     SystemEventBus& systemEventBus,
                     const std::function<void()>& markUiActivity);

/// GET /api/gps/observations wrapper with route-level policy callbacks.
void handleApiObservations(WebServer& server,
                           GpsObservationLog& gpsObservationLog,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity);

/// POST /api/gps/config wrapper with route-level policy callbacks.
void handleApiConfig(WebServer& server,
                     SettingsManager& settingsManager,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     LockoutLearner& lockoutLearner,
                     GpsObservationLog& gpsObservationLog,
                     PerfCounters& perfCounters,
                     SystemEventBus& systemEventBus,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

}  // namespace GpsApiService
