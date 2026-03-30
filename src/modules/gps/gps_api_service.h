#pragma once

#include <WebServer.h>

class GpsRuntimeModule;
class GpsObservationLog;
class SpeedSourceSelector;
class LockoutLearner;
class SettingsManager;
struct PerfCounters;
class SystemEventBus;

namespace GpsApiService {

/// GET /api/gps/status handler with route-level UI activity callback.
void handleApiStatus(WebServer& server,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     SettingsManager& settingsManager,
                     GpsObservationLog& gpsObservationLog,
                     LockoutLearner& lockoutLearner,
                     PerfCounters& perfCounters,
                     SystemEventBus& systemEventBus,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// GET /api/gps/observations handler with route-level policy callbacks.
void handleApiObservations(WebServer& server,
                           GpsObservationLog& gpsObservationLog,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// GET /api/gps/config handler with route-level UI activity callback.
void handleApiConfigGet(WebServer& server,
                        SettingsManager& settingsManager,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// POST /api/gps/config handler with route-level policy callbacks.
void handleApiConfig(WebServer& server,
                     SettingsManager& settingsManager,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     LockoutLearner& lockoutLearner,
                     GpsObservationLog& gpsObservationLog,
                     PerfCounters& perfCounters,
                     SystemEventBus& systemEventBus,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace GpsApiService
