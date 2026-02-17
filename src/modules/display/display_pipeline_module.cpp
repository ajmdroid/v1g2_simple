#include "display_pipeline_module.h"
#include "audio_beep.h"  // play_frequency_voice, play_direction_only, play_threat_escalation
#include "modules/camera/camera_runtime_module.h"
#include "perf_metrics.h"  // perfRecordDisplayRenderUs

void DisplayPipelineModule::begin(DisplayMode* displayModePtr,
                                  V1Display* displayPtr,
                                  PacketParser* parserPtr,
                                  SettingsManager* settingsMgr,
                                  V1BLEClient* bleClient,
                                  AlertPersistenceModule* alertPersistenceModule,
                                  VolumeFadeModule* volumeFadeModule,
                                  VoiceModule* voiceModule,
                                  SpeedVolumeModule* speedVolumeModule,
                                  DebugLogger* debugLogger) {
    displayMode = displayModePtr;
    display = displayPtr;
    parser = parserPtr;
    settings = settingsMgr;
    ble = bleClient;
    alertPersistence = alertPersistenceModule;
    volumeFade = volumeFadeModule;
    voice = voiceModule;
    speedVolume = speedVolumeModule;
    debug = debugLogger;
}

// Track lastAlertGapRecoverMs locally since it was removed from header
static unsigned long lastAlertGapRecoverMs = 0;

void DisplayPipelineModule::handleParsed(unsigned long nowMs, bool prioritySuppressed) {
    if (!display || !parser || !settings || !ble || !alertPersistence ||
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

    // Volume fade runs every frame — not gated by display draw throttle.
    // BLE restore commands must not be delayed by 25ms draw timing.
    {
        VolumeFadeContext fadeCtx;
        if (hasAlerts) {
            AlertData fadePriority = parser->getPriorityAlert();
            fadeCtx.hasAlert = true;
            fadeCtx.alertMuted = state.muted;
            fadeCtx.alertSuppressed = prioritySuppressed;
            fadeCtx.currentVolume = state.mainVolume;
            fadeCtx.currentMuteVolume = state.muteVolume;
            fadeCtx.currentFrequency = (uint16_t)fadePriority.frequency;
        } else {
            fadeCtx.hasAlert = false;
            fadeCtx.currentVolume = state.mainVolume;
            fadeCtx.currentMuteVolume = state.muteVolume;
            fadeCtx.currentFrequency = 0;
        }
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
    }

    if (nowMs - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) {
        PERF_INC(displaySkips);
        return;
    }
    lastDisplayDraw = nowMs;

    const V1Settings& alertSettings = settingsRef;

    if (hasAlerts) {
        AlertData priority = parser->getPriorityAlert();
        int alertCount = parser->getAlertCount();
        const auto& currentAlerts = parser->getAllAlerts();

        // Live V1 alerts own the screen/audio path and preempt camera UX.
        lastCameraVoiceStartTsMs = 0;
        lastCameraVoiceCameraId = 0;

        *displayMode = DisplayMode::LIVE;

        VoiceContext voiceCtx;
        voiceCtx.alerts = currentAlerts.data();
        voiceCtx.alertCount = alertCount;
        voiceCtx.priority = priority.isValid ? &priority : nullptr;
        voiceCtx.isMuted = state.muted;
        voiceCtx.isProxyConnected = ble->isProxyClientConnected();
        voiceCtx.mainVolume = state.mainVolume;
        voiceCtx.isSuppressed = prioritySuppressed;
        voiceCtx.now = nowMs;

        VoiceAction voiceAction = voice->process(voiceCtx);

        // Draw display FIRST (before audio) to eliminate perceived lag
        // User sees the alert card immediately, then hears the announcement
        if (debug) debug->notifyRenderState(true);  // Defer SD flush during render
        unsigned long startUs = micros();
        display->update(priority, currentAlerts.data(), alertCount, state);
        unsigned long endUs = micros();
        if (debug) debug->notifyRenderState(false);
        recordDisplayTiming("display.update(alerts)", startUs, endUs);
        recordPerfTiming("display.update(alerts)", startUs, endUs);

        // Play audio AFTER display update completes
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

        alertPersistence->setPersistedAlert(priority);

    } else {
        *displayMode = DisplayMode::IDLE;

        voice->clearAllState();
        // Note: Do NOT clear alertPersistence here - we need the stored alert for persistence display
        // Persistence is cleared on slot change (below) or when window expires

        speedVolume->reset();

        const V1Settings& s = settingsRef;
        uint8_t persistSec = settings->getSlotAlertPersistSec(s.activeSlot);

        static int lastPersistenceSlot = -1;
        if (s.activeSlot != lastPersistenceSlot) {
            lastPersistenceSlot = s.activeSlot;
            alertPersistence->clearPersistence();
        }

        if (persistSec > 0 && alertPersistence->getPersistedAlert().isValid) {
            // Persisted V1 alert remains higher priority than camera UX.
            lastCameraVoiceStartTsMs = 0;
            lastCameraVoiceCameraId = 0;
            alertPersistence->startPersistence(nowMs);

            unsigned long persistMs = persistSec * 1000UL;
            if (alertPersistence->shouldShowPersisted(nowMs, persistMs)) {
                if (debug) debug->notifyRenderState(true);
                unsigned long startUs = micros();
                display->updatePersisted(alertPersistence->getPersistedAlert(), state);
                unsigned long endUs = micros();
                if (debug) debug->notifyRenderState(false);
                recordDisplayTiming("display.persisted", startUs, endUs);
                recordPerfTiming("display.persisted", startUs, endUs);
            } else {
                // Persistence window expired - clear flag so isPersistenceActive() returns false
                PERF_INC(alertPersistExpires);
                alertPersistence->clearPersistence();
                if (debug) debug->notifyRenderState(true);
                unsigned long startUs = micros();
                display->update(state);
                unsigned long endUs = micros();
                if (debug) debug->notifyRenderState(false);
                recordDisplayTiming("display.resting", startUs, endUs);
                recordPerfTiming("display.resting", startUs, endUs);
            }
        } else {
            alertPersistence->clearPersistence();
            const CameraRuntimeStatus cameraStatus = cameraRuntimeModule.snapshot();
            const bool showCameraBanner =
                cameraStatus.enabled && cameraStatus.activeAlert.active && cameraStatus.activeAlert.type != 0;

            if (showCameraBanner) {
                const bool shouldAnnounceCamera =
                    !state.muted &&
                    !ble->isProxyClientConnected() &&
                    cameraStatus.activeAlert.startTsMs != 0 &&
                    (cameraStatus.activeAlert.startTsMs != lastCameraVoiceStartTsMs ||
                     cameraStatus.activeAlert.cameraId != lastCameraVoiceCameraId);
                if (shouldAnnounceCamera) {
                    play_camera_ahead_voice(cameraStatus.activeAlert.type);
                }
                lastCameraVoiceStartTsMs = cameraStatus.activeAlert.startTsMs;
                lastCameraVoiceCameraId = cameraStatus.activeAlert.cameraId;

                if (debug) debug->notifyRenderState(true);
                unsigned long startUs = micros();
                display->updateCameraAlert(cameraStatus.activeAlert.type, state.muted);
                unsigned long endUs = micros();
                if (debug) debug->notifyRenderState(false);
                recordDisplayTiming("display.camera", startUs, endUs);
                recordPerfTiming("display.camera", startUs, endUs);
            } else {
                lastCameraVoiceStartTsMs = 0;
                lastCameraVoiceCameraId = 0;
                if (debug) debug->notifyRenderState(true);
                unsigned long startUs = micros();
                display->update(state);
                unsigned long endUs = micros();
                if (debug) debug->notifyRenderState(false);
                recordDisplayTiming("display.resting", startUs, endUs);
                recordPerfTiming("display.resting", startUs, endUs);
            }
        }
    }
}

void DisplayPipelineModule::recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    unsigned long dur = endUs - startUs;
    displayLatencySum += dur;
    displayLatencyCount++;
    if (dur > displayLatencyMax) displayLatencyMax = dur;

    // Rate-limit SLOW logs to max 1/sec to prevent log spam during stalls
    static unsigned long lastSlowLogMs = 0;
    unsigned long nowMs = millis();
    if (dur > DISPLAY_SLOW_THRESHOLD_US && debug && debug->isEnabledFor(DebugLogCategory::Display)) {
        if (nowMs - lastSlowLogMs >= 1000) {
            debug->logf(DebugLogCategory::Display, "[SLOW] %s: %lums", label, dur / 1000);
            lastSlowLogMs = nowMs;
        }
    }

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
    unsigned long dur = endUs - startUs;
    
    // Always record to perf metrics for scorecard attribution
    perfRecordDisplayRenderUs(dur);
    PERF_INC(displayUpdates);
    
    if (!PERF_TIMING_LOGS) return;
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
