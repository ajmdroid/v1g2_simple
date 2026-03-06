#include "display_pipeline_module.h"
#include "audio_beep.h"  // play_frequency_voice, play_direction_only, play_threat_escalation
#include "perf_metrics.h"  // perfRecordDisplayRenderUs
#include "modules/camera_alert/camera_alert_module.h"
#include "modules/gps/gps_runtime_module.h"

#include <math.h>

void DisplayPipelineModule::begin(DisplayMode* displayModePtr,
                                  V1Display* displayPtr,
                                  PacketParser* parserPtr,
                                  SettingsManager* settingsMgr,
                                  V1BLEClient* bleClient,
                                  AlertPersistenceModule* alertPersistenceModule,
                                  VolumeFadeModule* volumeFadeModule,
                                  VoiceModule* voiceModule,
                                  GpsRuntimeModule* gpsModulePtr,
                                  CameraAlertModule* cameraModulePtr) {
    displayMode = displayModePtr;
    display = displayPtr;
    parser = parserPtr;
    settings = settingsMgr;
    ble = bleClient;
    alertPersistence = alertPersistenceModule;
    volumeFade = volumeFadeModule;
    voice = voiceModule;
    gpsModule = gpsModulePtr;
    cameraModule = cameraModulePtr;
    cameraAlertActive_ = false;
    lastPersistenceSlot = -1;
    lastRenderedOwner_ = RenderOwner::Unknown;
}

// Track lastAlertGapRecoverMs locally since it was removed from header
static unsigned long lastAlertGapRecoverMs = 0;

void DisplayPipelineModule::processCameraState(uint32_t nowMs) {
    if (!cameraModule || !gpsModule) {
        return;
    }

    const GpsRuntimeStatus gpsStatus = gpsModule->snapshot(nowMs);
    CameraAlertContext ctx;
    ctx.gpsValid = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        ctx.latE5 = static_cast<int32_t>(lroundf(gpsStatus.latitudeDeg * 100000.0f));
        ctx.lonE5 = static_cast<int32_t>(lroundf(gpsStatus.longitudeDeg * 100000.0f));
    }
    ctx.speedMph = gpsStatus.speedMph;
    ctx.courseValid = gpsStatus.courseValid;
    ctx.courseDeg = gpsStatus.courseDeg;
    ctx.courseAgeMs = gpsStatus.courseAgeMs;
    cameraModule->process(nowMs, ctx);
}

void DisplayPipelineModule::dispatchCameraVoice() {
    if (!cameraModule) {
        return;
    }

    CameraVoiceEvent event;
    if (!cameraModule->consumePendingVoice(event)) {
        return;
    }

    const bool playbackStarted = play_camera_alert(event.type, event.isNearStage);
    cameraModule->onVoicePlaybackResult(event, playbackStarted);
}

void DisplayPipelineModule::renderIdleOwner(uint32_t nowMs,
                                            const DisplayState& state,
                                            bool forceRedraw) {
    const V1Settings& s = settings->get();
    const uint8_t persistSec = settings->getSlotAlertPersistSec(s.activeSlot);

    if (s.activeSlot != lastPersistenceSlot) {
        lastPersistenceSlot = s.activeSlot;
        alertPersistence->clearPersistence();
    }

    if (persistSec > 0 && alertPersistence->getPersistedAlert().isValid) {
        alertPersistence->startPersistence(nowMs);

        const unsigned long persistMs = persistSec * 1000UL;
        if (alertPersistence->shouldShowPersisted(nowMs, persistMs)) {
            if (forceRedraw || lastRenderedOwner_ != RenderOwner::Persisted) {
                display->forceNextRedraw();
            }

            const unsigned long startUs = micros();
            display->updatePersisted(alertPersistence->getPersistedAlert(), state);
            const unsigned long endUs = micros();
            recordDisplayTiming("display.persisted", startUs, endUs);
            recordPerfTiming("display.persisted", startUs, endUs);
            cameraAlertActive_ = false;
            lastRenderedOwner_ = RenderOwner::Persisted;
            return;
        }

        PERF_INC(alertPersistExpires);
        alertPersistence->clearPersistence();
    } else {
        alertPersistence->clearPersistence();
    }

    if (cameraModule && cameraModule->isDisplayActive()) {
        if (forceRedraw || lastRenderedOwner_ != RenderOwner::Camera) {
            display->forceNextRedraw();
        }

        const unsigned long startUs = micros();
        display->updateCameraAlert(cameraModule->displayPayload(), state);
        const unsigned long endUs = micros();
        recordDisplayTiming("display.camera", startUs, endUs);
        recordPerfTiming("display.camera", startUs, endUs);
        cameraAlertActive_ = true;
        lastRenderedOwner_ = RenderOwner::Camera;
        return;
    }

    if (forceRedraw || lastRenderedOwner_ != RenderOwner::Resting) {
        display->forceNextRedraw();
    }

    const unsigned long startUs = micros();
    display->update(state);
    const unsigned long endUs = micros();
    recordDisplayTiming("display.resting", startUs, endUs);
    recordPerfTiming("display.resting", startUs, endUs);
    cameraAlertActive_ = false;
    lastRenderedOwner_ = RenderOwner::Resting;
}

void DisplayPipelineModule::handleParsed(unsigned long nowMs, bool prioritySuppressed) {
    if (!display || !parser || !settings || !ble || !alertPersistence ||
        !volumeFade || !voice || !displayMode) {
        cameraAlertActive_ = false;
        return;
    }

    DisplayState state = parser->getDisplayState();
    bool hasAlerts = parser->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority =
        hasAlerts && parser->getRenderablePriorityAlert(priority);
    if (hasRenderablePriority) {
        const AlertData rawPriority = parser->getPriorityAlert();
        const bool rawRenderable = rawPriority.isValid &&
                                   rawPriority.band != BAND_NONE &&
                                   ((rawPriority.band == BAND_LASER) ||
                                    (rawPriority.frequency != 0));
        if (!rawRenderable) {
            PERF_INC(displayLiveFallbackToUsable);
        }
    }
    const V1Settings& settingsRef = settings->get();

    if (!hasAlerts && state.activeBands != BAND_NONE) {
        unsigned long gapNow = nowMs;
        if (gapNow - lastAlertGapRecoverMs > 50) {
            // Preserve partially assembled alert rows; parser freshness/timeout
            // guards handle stale data without discarding in-progress tables.
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
            fadeCtx.hasAlert = true;
            fadeCtx.alertMuted = state.muted;
            fadeCtx.alertSuppressed = prioritySuppressed;
            fadeCtx.currentVolume = state.mainVolume;
            fadeCtx.currentMuteVolume = state.muteVolume;
            fadeCtx.currentFrequency = hasRenderablePriority ? static_cast<uint16_t>(priority.frequency) : 0;
        } else {
            fadeCtx.hasAlert = false;
            fadeCtx.currentVolume = state.mainVolume;
            fadeCtx.currentMuteVolume = state.muteVolume;
            fadeCtx.currentFrequency = 0;
        }
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

    if (!hasAlerts) {
        processCameraState(nowMs);
        dispatchCameraVoice();
    }

    if (nowMs - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) {
        PERF_INC(displaySkips);
        return;
    }
    lastDisplayDraw = nowMs;

    const V1Settings& alertSettings = settingsRef;

    if (hasAlerts) {
        int alertCount = parser->getAlertCount();
        const auto& currentAlerts = parser->getAllAlerts();

        // Live V1 alerts own the screen/audio path.

        *displayMode = DisplayMode::LIVE;
        cameraAlertActive_ = false;

        VoiceContext voiceCtx;
        voiceCtx.alerts = currentAlerts.data();
        voiceCtx.alertCount = alertCount;
        voiceCtx.priority = hasRenderablePriority ? &priority : nullptr;
        voiceCtx.isMuted = state.muted;
        voiceCtx.isProxyConnected = ble->isProxyClientConnected();
        voiceCtx.mainVolume = state.mainVolume;
        voiceCtx.isSuppressed = prioritySuppressed;
        voiceCtx.now = nowMs;

        VoiceAction voiceAction = voice->process(voiceCtx);

        // Draw display FIRST (before audio) to eliminate perceived lag
        // User sees the alert card immediately, then hears the announcement
        unsigned long startUs = micros();
        if (hasRenderablePriority) {
            display->update(priority, currentAlerts.data(), alertCount, state);
        } else {
            display->update(state);
        }
        unsigned long endUs = micros();
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

        if (hasRenderablePriority) {
            alertPersistence->setPersistedAlert(priority);
        }
        lastRenderedOwner_ = RenderOwner::Live;

    } else {
        *displayMode = DisplayMode::IDLE;

        voice->clearAllState();
        renderIdleOwner(nowMs, state, false);
    }
}

void DisplayPipelineModule::restoreCurrentOwner(uint32_t nowMs) {
    if (!display || !parser || !settings || !ble || !alertPersistence ||
        !volumeFade || !voice || !displayMode) {
        cameraAlertActive_ = false;
        return;
    }

    display->forceNextRedraw();

    if (!ble->isConnected()) {
        display->showScanning();
        *displayMode = DisplayMode::IDLE;
        cameraAlertActive_ = false;
        lastRenderedOwner_ = RenderOwner::Scanning;
        return;
    }

    const DisplayState state = parser->getDisplayState();
    const bool hasAlerts = parser->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority =
        hasAlerts && parser->getRenderablePriorityAlert(priority);

    if (hasAlerts) {
        *displayMode = DisplayMode::LIVE;
        cameraAlertActive_ = false;
        if (hasRenderablePriority) {
            const auto& alerts = parser->getAllAlerts();
            display->update(priority, alerts.data(), parser->getAlertCount(), state);
        } else {
            display->update(state);
        }
        lastRenderedOwner_ = RenderOwner::Live;
        return;
    }

    *displayMode = DisplayMode::IDLE;
    processCameraState(nowMs);
    renderIdleOwner(nowMs, state, true);
}

void DisplayPipelineModule::recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    unsigned long dur = endUs - startUs;
    displayLatencySum += dur;
    displayLatencyCount++;
    if (dur > displayLatencyMax) displayLatencyMax = dur;

    unsigned long nowMs = millis();

    if ((nowMs - displayLatencyLastLog) > DISPLAY_LOG_INTERVAL_MS && displayLatencyCount > 0) {
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
