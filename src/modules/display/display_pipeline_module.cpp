#include "display_pipeline_module.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "perf_metrics.h"  // perfRecordDisplayRenderUs

void DisplayPipelineModule::begin(DisplayMode* displayModePtr,
                                  V1Display* displayPtr,
                                  PacketParser* parserPtr,
                                  SettingsManager* settingsMgr,
                                  V1BLEClient* bleClient,
                                  AlertPersistenceModule* alertPersistenceModule,
                                  VoiceModule* voiceModule) {
    displayMode = displayModePtr;
    display = displayPtr;
    parser = parserPtr;
    settings = settingsMgr;
    ble = bleClient;
    alertPersistence = alertPersistenceModule;
    voice = voiceModule;
    lastPersistenceSlot = -1;
    lastRenderedOwner_ = RenderOwner::Unknown;
    lastAlertGapRecoverMs = 0;
}

void DisplayPipelineModule::renderIdleOwner(uint32_t nowMs,
                                            const DisplayState& state,
                                            bool forceRedraw,
                                            bool restoreContext) {
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

            perfSetDisplayRenderScenario(restoreContext ? PerfDisplayRenderScenario::Restore
                                                        : PerfDisplayRenderScenario::Persisted);
            const unsigned long startUs = micros();
            display->updatePersisted(alertPersistence->getPersistedAlert(), state);
            const unsigned long endUs = micros();
            recordDisplayTiming("display.persisted", startUs, endUs);
            recordPerfTiming("display.persisted", startUs, endUs);
            perfClearDisplayRenderScenario();
            lastRenderedOwner_ = RenderOwner::Persisted;
            return;
        }

        PERF_INC(alertPersistExpires);
        alertPersistence->clearPersistence();
    } else {
        alertPersistence->clearPersistence();
    }

    if (forceRedraw || lastRenderedOwner_ != RenderOwner::Resting) {
        display->forceNextRedraw();
    }

    perfSetDisplayRenderScenario(restoreContext ? PerfDisplayRenderScenario::Restore
                                                : PerfDisplayRenderScenario::Resting);
    const unsigned long startUs = micros();
    display->update(state);
    const unsigned long endUs = micros();
    recordDisplayTiming("display.resting", startUs, endUs);
    recordPerfTiming("display.resting", startUs, endUs);
    perfClearDisplayRenderScenario();
    lastRenderedOwner_ = RenderOwner::Resting;
}

void DisplayPipelineModule::handleParsed(unsigned long nowMs, bool prioritySuppressed) {
    if (!display || !parser || !settings || !ble || !alertPersistence ||
        !voice || !displayMode) {
        return;
    }

    DisplayState state = parser->getDisplayState();
    const bool hasAlerts = parser->hasAlerts();
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
        const unsigned long gapNow = nowMs;
        if (gapNow - lastAlertGapRecoverMs > 50) {
            // Preserve partially assembled alert rows; parser freshness/timeout
            // guards handle stale data without discarding in-progress tables.
            const unsigned long gapStartUs = micros();
            ble->requestAlertData();
            perfRecordDisplayGapRecoverUs(micros() - gapStartUs);
            lastAlertGapRecoverMs = gapNow;
        }
    }

    const unsigned long muteNow = nowMs;
    if (state.muted != debouncedMuteState) {
        if (muteNow - lastMuteChangeMs > MUTE_DEBOUNCE_MS) {
            debouncedMuteState = state.muted;
            lastMuteChangeMs = muteNow;
        } else {
            state.muted = debouncedMuteState;
        }
    }

    if (nowMs - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) {
        PERF_INC(displaySkips);
        return;
    }
    lastDisplayDraw = nowMs;

    if (hasAlerts) {
        const int alertCount = parser->getAlertCount();
        const auto& currentAlerts = parser->getAllAlerts();
        const bool deferSecondaryCards = ble->isConnectBurstSettling();
        const AlertData* renderAlerts = currentAlerts.data();
        int renderAlertCount = alertCount;
        AlertData priorityOnlyAlert[1];
        if (deferSecondaryCards && hasRenderablePriority) {
            priorityOnlyAlert[0] = priority;
            renderAlerts = priorityOnlyAlert;
            renderAlertCount = 1;
        }

        *displayMode = DisplayMode::LIVE;

        VoiceContext voiceCtx;
        voiceCtx.alerts = currentAlerts.data();
        voiceCtx.alertCount = alertCount;
        voiceCtx.priority = hasRenderablePriority ? &priority : nullptr;
        voiceCtx.isMuted = state.muted;
        voiceCtx.isProxyConnected = ble->isProxyClientConnected();
        voiceCtx.mainVolume = state.mainVolume;
        voiceCtx.isSuppressed = prioritySuppressed;
        voiceCtx.now = nowMs;

        // Speed mute: suppress voice if low-speed mute active & band not overridden
        if (!voiceCtx.isSuppressed && speedMute) {
            const auto& smState = speedMute->getState();
            if (smState.muteActive && hasRenderablePriority) {
                if (!speedMute->isBandOverridden(priority.band)) {
                    voiceCtx.isSuppressed = true;
                }
            }
        }

        // Speed mute band override: when speed-mute lowered vol to 0 the parser
        // sets state.muted = true.  That isMuted flag (and muteVoiceIfVolZero)
        // would kill voice for ALL bands — including Laser/Ka — before the
        // isSuppressed band check above is reached.  Clear the false mute for
        // bands that are overridden so the voice module can announce them.
        if (speedMute && hasRenderablePriority) {
            const auto& smState = speedMute->getState();
            if (smState.muteActive && speedMute->isBandOverridden(priority.band)) {
                voiceCtx.isMuted = false;
                if (voiceCtx.mainVolume == 0) {
                    voiceCtx.mainVolume = 1;   // Prevent muteVoiceIfVolZero kill
                }
            }
        }

        const unsigned long voiceStartUs = micros();
        const VoiceAction voiceAction = voice->process(voiceCtx);
        perfRecordDisplayVoiceUs(micros() - voiceStartUs);

        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Live);
        const unsigned long startUs = micros();
        if (hasRenderablePriority) {
            display->update(priority, renderAlerts, renderAlertCount, state);
        } else {
            display->update(state);
        }
        const unsigned long endUs = micros();
        recordDisplayTiming("display.update(alerts)", startUs, endUs);
        recordPerfTiming("display.update(alerts)", startUs, endUs);
        perfClearDisplayRenderScenario();

        if (voiceAction.hasAction()) {
            switch (voiceAction.type) {
                case VoiceAction::Type::ANNOUNCE_PRIORITY:
                    play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                         settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled,
                                         voiceAction.bogeyCount);
                    break;
                case VoiceAction::Type::ANNOUNCE_DIRECTION:
                    play_direction_only(voiceAction.dir, voiceAction.bogeyCount);
                    break;
                case VoiceAction::Type::ANNOUNCE_SECONDARY:
                    play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                         settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled, 1);
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
        return;
    }

    *displayMode = DisplayMode::IDLE;
    voice->clearAllState();
    renderIdleOwner(nowMs, state, false, false);
}

void DisplayPipelineModule::restoreCurrentOwner(uint32_t nowMs) {
    if (!display || !parser || !settings || !ble || !alertPersistence ||
        !voice || !displayMode) {
        return;
    }

    display->forceNextRedraw();

    if (!ble->isConnected()) {
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        display->showScanning();
        perfClearDisplayRenderScenario();
        *displayMode = DisplayMode::IDLE;
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
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        const unsigned long startUs = micros();
        if (hasRenderablePriority) {
            const auto& alerts = parser->getAllAlerts();
            display->update(priority, alerts.data(), parser->getAlertCount(), state);
        } else {
            display->update(state);
        }
        perfRecordDisplayScenarioRenderUs(micros() - startUs);
        perfClearDisplayRenderScenario();
        lastRenderedOwner_ = RenderOwner::Live;
        return;
    }

    *displayMode = DisplayMode::IDLE;
    renderIdleOwner(nowMs, state, true, true);
}

bool DisplayPipelineModule::allowsObdPairGesture(uint32_t nowMs) const {
    if (!displayMode || !parser || !settings || !alertPersistence) {
        return false;
    }

    if (*displayMode != DisplayMode::IDLE) {
        return false;
    }

    if (parser->hasAlerts()) {
        return false;
    }

    const V1Settings& s = settings->get();
    const uint8_t persistSec = settings->getSlotAlertPersistSec(s.activeSlot);
    if (persistSec > 0 &&
        alertPersistence->getPersistedAlert().isValid &&
        alertPersistence->shouldShowPersisted(nowMs, persistSec * 1000UL)) {
        return false;
    }

    return true;
}

void DisplayPipelineModule::recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    const unsigned long dur = endUs - startUs;
    displayLatencySum += dur;
    displayLatencyCount++;
    if (dur > displayLatencyMax) displayLatencyMax = dur;

    const unsigned long nowMs = millis();

    if ((nowMs - displayLatencyLastLog) > DISPLAY_LOG_INTERVAL_MS && displayLatencyCount > 0) {
        displayLatencySum = 0;
        displayLatencyCount = 0;
        displayLatencyMax = 0;
        displayLatencyLastLog = nowMs;
    }
}

void DisplayPipelineModule::recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    const unsigned long dur = endUs - startUs;

    // Always record to perf metrics for scorecard attribution.
    perfRecordDisplayRenderUs(dur);
    perfRecordDisplayScenarioRenderUs(dur);
    PERF_INC(displayUpdates);

    if (!PERF_TIMING_LOGS) return;
    perfTimingAccum += dur;
    perfTimingCount++;
    if (dur > perfTimingMax) perfTimingMax = dur;
    const unsigned long nowMs = millis();
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
