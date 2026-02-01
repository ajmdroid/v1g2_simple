#include "display_pipeline_module.h"
#include "audio_beep.h"  // play_frequency_voice, play_direction_only, play_threat_escalation

void DisplayPipelineModule::begin(DisplayMode* displayModePtr,
                                  V1Display* displayPtr,
                                  PacketParser* parserPtr,
                                  SettingsManager* settingsMgr,
                                  GPSHandler* gpsHandler,
                                  LockoutManager* lockouts,
                                  AutoLockoutManager* autoLockouts,
                                  V1BLEClient* bleClient,
                                  CameraAlertModule* cameraAlertModule,
                                  AlertPersistenceModule* alertPersistenceModule,
                                  VolumeFadeModule* volumeFadeModule,
                                  VoiceModule* voiceModule,
                                  SpeedVolumeModule* speedVolumeModule,
                                  DebugLogger* debugLogger) {
    displayMode = displayModePtr;
    display = displayPtr;
    parser = parserPtr;
    settings = settingsMgr;
    gps = gpsHandler;
    lockoutMgr = lockouts;
    autoLockoutMgr = autoLockouts;
    ble = bleClient;
    cameraAlert = cameraAlertModule;
    alertPersistence = alertPersistenceModule;
    volumeFade = volumeFadeModule;
    voice = voiceModule;
    speedVolume = speedVolumeModule;
    debug = debugLogger;
}

void DisplayPipelineModule::handleParsed(unsigned long nowMs) {
    if (!display || !parser || !settings || !cameraAlert || !alertPersistence ||
        !volumeFade || !voice || !speedVolume || !displayMode) {
        return;
    }

    DisplayState state = parser->getDisplayState();
    bool hasAlerts = parser->hasAlerts();
    const V1Settings& settingsRef = settings->get();

    if (!hasAlerts && state.activeBands != BAND_NONE) {
        unsigned long gapNow = nowMs;
        if (gapNow - lastAlertGapRecoverMs > 50) {
            parser->resetAlertAssembly();
            ble->requestAlertData();
            lastAlertGapRecoverMs = gapNow;
        }
    }

    unsigned long muteNow = nowMs;
    if (state.muted != debouncedMuteState) {
        if (muteNow - lastMuteChangeMs > MUTE_DEBOUNCE_MS) {
            debouncedMuteState = state.muted;
            lastMuteChangeMs = muteNow;
        } else {
            state.muted = debouncedMuteState;
        }
    }

    if (nowMs - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) {
        return;
    }
    lastDisplayDraw = nowMs;

    const V1Settings& alertSettings = settingsRef;

    if (hasAlerts) {
        AlertData priority = parser->getPriorityAlert();
        int alertCount = parser->getAlertCount();
        const auto& currentAlerts = parser->getAllAlerts();

        *displayMode = DisplayMode::LIVE;

        bool priorityInLockout = false;

        // Log alert visibility for lockout debugging
        bool gpsReady = gps && gps->isReadyForNavigation();
        bool lockoutSystemReady = gpsReady && lockoutMgr && autoLockoutMgr;

        if (priority.isValid && priority.band != BAND_NONE && debug && debug->isEnabledFor(DebugLogCategory::Lockout)) {
            const char* bandStr = (priority.band == BAND_X) ? "X" : (priority.band == BAND_K) ? "K" : 
                                  (priority.band == BAND_KA) ? "Ka" : "Laser";
            uint8_t strength = VoiceModule::getAlertBars(priority);
            if (!lockoutSystemReady) {
                const char* reason = !gps ? "no GPS" : !gps->isReadyForNavigation() ? "GPS not ready" : 
                                     !lockoutMgr ? "no lockoutMgr" : "no autoLockoutMgr";
                debug->logf(DebugLogCategory::Lockout, "[Lockout] Alert: %s %.3fMHz str=%d → SKIPPED (%s)",
                           bandStr, priority.frequency/1000.0f, strength, reason);
            }
        }

        if (lockoutSystemReady) {
            GPSFix fix = gps->getFix();

            if (priority.isValid && priority.band != BAND_NONE) {
                priorityInLockout = lockoutMgr->shouldMuteAlert(fix.latitude, fix.longitude, priority.band);
                uint32_t currentAlertId = VoiceModule::makeAlertId(priority.band, (uint16_t)priority.frequency);

                if (priorityInLockout && !state.muted) {
                    if (!lockoutMuteSent || currentAlertId != lastLockoutAlertId) {
                        ble->setMute(true);
                        lockoutMuteSent = true;
                        lastLockoutAlertId = currentAlertId;
                        display->setLockoutMuted(true);
                    }
                } else if (!priorityInLockout) {
                    lockoutMuteSent = false;
                    display->setLockoutMuted(false);
                }

                bool isMoving = gps->isMoving();
                uint8_t strength = VoiceModule::getAlertBars(priority);
                float heading = gps->getSmoothedHeading();
                autoLockoutMgr->recordAlert(fix.latitude, fix.longitude, priority.band,
                                            (uint32_t)priority.frequency, strength, 0, isMoving, heading);
            }

            for (int i = 0; i < alertCount; i++) {
                const AlertData& alert = currentAlerts[i];
                if (!alert.isValid || alert.band == BAND_NONE) continue;
                if (alert.band == priority.band && alert.frequency == priority.frequency) continue;

                uint8_t strength = VoiceModule::getAlertBars(alert);
                float heading = gps->getSmoothedHeading();
                autoLockoutMgr->recordAlert(fix.latitude, fix.longitude, alert.band,
                                            (uint32_t)alert.frequency, strength, 0, gps->isMoving(), heading);
            }
        }

        VolumeFadeContext fadeCtx;
        fadeCtx.hasAlert = true;
        fadeCtx.alertMuted = state.muted;
        fadeCtx.alertInLockout = priorityInLockout;
        fadeCtx.currentVolume = state.mainVolume;
        fadeCtx.currentMuteVolume = state.muteVolume;
        fadeCtx.currentFrequency = (uint16_t)priority.frequency;
        fadeCtx.speedBoostActive = speedVolume->isBoostActive();
        fadeCtx.speedBoostOriginalVolume = speedVolume->getOriginalVolume();
        fadeCtx.now = nowMs;

        VolumeFadeAction fadeAction = volumeFade->process(fadeCtx);
        if (fadeAction.hasAction()) {
            if (fadeAction.type == VolumeFadeAction::Type::FADE_DOWN) {
                ble->setVolume(fadeAction.targetVolume, fadeAction.targetMuteVolume);
            } else if (fadeAction.type == VolumeFadeAction::Type::RESTORE) {
                ble->setVolume(fadeAction.restoreVolume, fadeAction.restoreMuteVolume);
            }
        }

        VoiceContext voiceCtx;
        voiceCtx.alerts = currentAlerts.data();
        voiceCtx.alertCount = alertCount;
        voiceCtx.priority = priority.isValid ? &priority : nullptr;
        voiceCtx.isMuted = state.muted;
        voiceCtx.isProxyConnected = ble->isProxyClientConnected();
        voiceCtx.mainVolume = state.mainVolume;
        voiceCtx.isInLockout = priorityInLockout;
        voiceCtx.now = nowMs;

        VoiceAction voiceAction = voice->process(voiceCtx);

        if (voiceAction.hasAction()) {
            switch (voiceAction.type) {
                case VoiceAction::Type::ANNOUNCE_PRIORITY:
                    play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                         alertSettings.voiceAlertMode, alertSettings.voiceDirectionEnabled,
                                         voiceAction.bogeyCount);
                    break;
                case VoiceAction::Type::ANNOUNCE_DIRECTION:
                    play_direction_only(voiceAction.dir, voiceAction.bogeyCount);
                    break;
                case VoiceAction::Type::ANNOUNCE_SECONDARY:
                    play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                         alertSettings.voiceAlertMode, alertSettings.voiceDirectionEnabled, 1);
                    break;
                case VoiceAction::Type::ANNOUNCE_ESCALATION:
                    play_threat_escalation(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                           voiceAction.bogeyCount, voiceAction.aheadCount,
                                           voiceAction.behindCount, voiceAction.sideCount);
                    break;
                case VoiceAction::Type::NONE:
                default:
                    break;
            }
        }

        cameraAlert->updateCardStateForV1(true);
        unsigned long startUs = micros();
        display->update(priority, currentAlerts.data(), alertCount, state);
        unsigned long endUs = micros();
        recordDisplayTiming("display.update(alerts)", startUs, endUs);
        recordPerfTiming("display.update(alerts)", startUs, endUs);

        alertPersistence->setPersistedAlert(priority);

    } else {
        *displayMode = DisplayMode::IDLE;

        if (gps && gps->hasValidFix() && gps->isMoving() && autoLockoutMgr) {
            static unsigned long lastPassthroughRecordMs = 0;
            if (nowMs - lastPassthroughRecordMs > 5000) {
                GPSFix fix = gps->getFix();
                autoLockoutMgr->recordPassthrough(fix.latitude, fix.longitude);
                lastPassthroughRecordMs = nowMs;
            }
        }

        voice->clearAllState();
        // Note: Do NOT clear alertPersistence here - we need the stored alert for persistence display
        // Persistence is cleared on slot change (below) or when window expires

        const DisplayState& restoreState = parser->getDisplayState();
        VolumeFadeContext fadeCtx;
        fadeCtx.hasAlert = false;
        fadeCtx.alertMuted = false;
        fadeCtx.alertInLockout = false;
        fadeCtx.currentVolume = restoreState.mainVolume;
        fadeCtx.currentMuteVolume = restoreState.muteVolume;
        fadeCtx.currentFrequency = 0;
        fadeCtx.speedBoostActive = speedVolume->isBoostActive();
        fadeCtx.speedBoostOriginalVolume = speedVolume->getOriginalVolume();
        fadeCtx.now = nowMs;

        VolumeFadeAction fadeAction = volumeFade->process(fadeCtx);
        if (fadeAction.hasAction() && fadeAction.type == VolumeFadeAction::Type::RESTORE) {
            ble->setVolume(fadeAction.restoreVolume, fadeAction.restoreMuteVolume);
        }

        speedVolume->reset();

        const V1Settings& s = settingsRef;
        uint8_t persistSec = settings->getSlotAlertPersistSec(s.activeSlot);

        static int lastPersistenceSlot = -1;
        if (s.activeSlot != lastPersistenceSlot) {
            lastPersistenceSlot = s.activeSlot;
            alertPersistence->clearPersistence();
        }

        if (persistSec > 0 && alertPersistence->getPersistedAlert().isValid) {
            alertPersistence->startPersistence(nowMs);

            unsigned long persistMs = persistSec * 1000UL;
            if (alertPersistence->shouldShowPersisted(nowMs, persistMs)) {
                unsigned long startUs = micros();
                display->updatePersisted(alertPersistence->getPersistedAlert(), state);
                unsigned long endUs = micros();
                recordDisplayTiming("display.persisted", startUs, endUs);
                recordPerfTiming("display.persisted", startUs, endUs);
            } else {
                // Persistence window expired - clear flag so isPersistenceActive() returns false
                alertPersistence->clearPersistence();
                cameraAlert->updateCardStateForV1(false);
                unsigned long startUs = micros();
                display->update(state);
                unsigned long endUs = micros();
                recordDisplayTiming("display.resting", startUs, endUs);
                recordPerfTiming("display.resting", startUs, endUs);
            }
        } else {
            alertPersistence->clearPersistence();
            cameraAlert->updateCardStateForV1(false);
            unsigned long startUs = micros();
            display->update(state);
            unsigned long endUs = micros();
            recordDisplayTiming("display.resting", startUs, endUs);
            recordPerfTiming("display.resting", startUs, endUs);
        }
    }
}

void DisplayPipelineModule::recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    unsigned long dur = endUs - startUs;
    displayLatencySum += dur;
    displayLatencyCount++;
    if (dur > displayLatencyMax) displayLatencyMax = dur;

    if (dur > DISPLAY_SLOW_THRESHOLD_US && debug && debug->isEnabledFor(DebugLogCategory::Display)) {
        debug->logf(DebugLogCategory::Display, "[SLOW] %s: %lums", label, dur / 1000);
    }

    unsigned long nowMs = millis();
    if ((nowMs - displayLatencyLastLog) > DISPLAY_LOG_INTERVAL_MS && displayLatencyCount > 0) {
        if (debug && debug->isEnabledFor(DebugLogCategory::Display)) {
            debug->logf(DebugLogCategory::Display, "Display: avg=%luus max=%luus n=%lu",
                        displayLatencySum / displayLatencyCount, displayLatencyMax, displayLatencyCount);
        }
        displayLatencySum = 0;
        displayLatencyCount = 0;
        displayLatencyMax = 0;
        displayLatencyLastLog = nowMs;
    }
}

void DisplayPipelineModule::recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    if (!PERF_TIMING_LOGS) return;
    unsigned long dur = endUs - startUs;
    perfTimingAccum += dur;
    perfTimingCount++;
    if (dur > perfTimingMax) perfTimingMax = dur;
    unsigned long nowMs = millis();
    if (nowMs - perfLastReport > 5000) {
        Serial.printf("[PERF] %s: avg=%luus max=%luus (n=%lu)\n",
                      label,
                      perfTimingAccum / perfTimingCount,
                      perfTimingMax,
                      perfTimingCount);
        perfTimingAccum = 0;
        perfTimingCount = 0;
        perfTimingMax = 0;
        perfLastReport = nowMs;
    }
}
