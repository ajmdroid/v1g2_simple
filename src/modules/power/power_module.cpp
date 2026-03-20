#include "power_module.h"
#ifndef UNIT_TEST
#include "perf_metrics.h"
#define POWER_PERF_INC(counter) PERF_INC(counter)
#define POWER_PERF_FLUSH_NOW() do { (void)perfMetricsEnqueueSnapshotNow(); } while (0)
#else
#define POWER_PERF_INC(counter) do { } while (0)
#define POWER_PERF_FLUSH_NOW() do { } while (0)
#endif

void PowerModule::performShutdownRequest() {
    if (shutdownPreparationCallback) {
        shutdownPreparationCallback(shutdownPreparationContext);
    }

    if (display) {
        display->showShutdown();
        delay(1000);
    }

    if (battery) {
        battery->powerOff();
    }
}

void PowerModule::begin(BatteryManager* batteryMgr,
                        V1Display* disp,
                        SettingsManager* settingsMgr) {
    battery = batteryMgr;
    display = disp;
    settings = settingsMgr;
}

void PowerModule::setShutdownPreparationCallback(ShutdownPreparationCallback callback, void* context) {
    shutdownPreparationCallback = callback;
    shutdownPreparationContext = context;
}

void PowerModule::logStartupStatus() {
    if (!battery) return;
    Serial.printf("[Battery] Power source: %s\n",
                  battery->isOnBattery() ? "BATTERY" : "USB");
    Serial.printf("[Battery] Icon display: %s\n",
                  battery->hasBattery() ? "YES" : "NO");
    if (battery->hasBattery()) {
        Serial.printf("[Battery] Voltage: %dmV (%d%%)\n",
                      battery->getVoltageMillivolts(),
                      battery->getPercentage());
    }
}

void PowerModule::onV1DataReceived() {
    if (!autoPowerOffArmed) {
        autoPowerOffArmed = true;
        POWER_PERF_INC(powerAutoPowerArmed);
        Serial.println("[AutoPowerOff] Armed - V1 data received");
    }
}

void PowerModule::onV1ConnectionChange(bool connected) {
    if (!battery || !settings) return;

    if (connected) {
        if (autoPowerOffTimerStart != 0) {
            Serial.println("[AutoPowerOff] Timer cancelled - V1 reconnected");
            autoPowerOffTimerStart = 0;
            POWER_PERF_INC(powerAutoPowerTimerCancel);
        }
        return;
    }

    // On disconnect, start auto power-off timer if armed and configured.
    const V1Settings& s = settings->get();
    if (autoPowerOffArmed && s.autoPowerOffMinutes > 0) {
        autoPowerOffTimerStart = millis();
        POWER_PERF_INC(powerAutoPowerTimerStart);
        Serial.printf("[AutoPowerOff] Timer started: %d minutes\n", s.autoPowerOffMinutes);
    }
}

void PowerModule::process(unsigned long nowMs) {
    if (!battery || !display || !settings) return;

    battery->update();
    if (battery->processPowerButton()) {
        performShutdownRequest();
        return;
    }

    // Critical battery handling (warning + shutdown)
    if (battery->isOnBattery() && battery->hasBattery()) {
        if (battery->isCritical()) {
            if (!lowBatteryWarningShown) {
                Serial.println("[Battery] CRITICAL - showing low battery warning");
                display->showLowBattery();
                lowBatteryWarningShown = true;
                criticalBatteryTime = nowMs;
                POWER_PERF_INC(powerCriticalWarn);
            } else if (nowMs - criticalBatteryTime > 5000) {
                Serial.println("[Battery] CRITICAL - auto shutdown to protect battery");
                POWER_PERF_INC(powerCriticalShutdown);
                POWER_PERF_FLUSH_NOW();
                performShutdownRequest();
                return;
            }
        } else {
            lowBatteryWarningShown = false;
        }
    }

    // Auto power-off timer check
    if (autoPowerOffTimerStart != 0) {
        const V1Settings& s = settings->get();
        unsigned long elapsedMs = nowMs - autoPowerOffTimerStart;
        unsigned long timeoutMs = (unsigned long)s.autoPowerOffMinutes * 60UL * 1000UL;
        if (elapsedMs >= timeoutMs) {
            Serial.printf("[AutoPowerOff] Timer expired after %d minutes - powering off\n", s.autoPowerOffMinutes);
            autoPowerOffTimerStart = 0;
            POWER_PERF_INC(powerAutoPowerTimerExpire);
            POWER_PERF_FLUSH_NOW();
            performShutdownRequest();
            return;
        }
    }
}
