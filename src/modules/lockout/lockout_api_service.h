#pragma once

#include <Arduino.h>
#include <WebServer.h>

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

}  // namespace LockoutApiService
