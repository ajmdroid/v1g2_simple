#pragma once

#include <Arduino.h>
#include <WebServer.h>

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

}  // namespace GpsApiService
