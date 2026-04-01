#include "display_pipeline_module.h"
#include "display.h"
#include "packet_parser.h"
#include "display_mode.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/voice/voice_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/quiet/quiet_coordinator_voice_templates.h"
#include "perf_metrics.h"  // perfRecordDisplayRenderUs
#include "settings.h"
#include "ble_client.h"
#include "modules/alert_persistence/alert_persistence_module.h"

void DisplayPipelineModule::begin(DisplayMode* displayModePtr,
                                  V1Display* displayPtr,
                                  PacketParser* parserPtr,
                                  SettingsManager* settingsMgr,
                                  V1BLEClient* bleClient,
                                  AlertPersistenceModule* alertPersistenceModule,
                                  VoiceModule* voiceModule,
                                  QuietCoordinatorModule* quietCoordinator) {
    displayMode_ = displayModePtr;
    display_ = displayPtr;
    parser_ = parserPtr;
    settings_ = settingsMgr;
    ble_ = bleClient;
    alertPersistence_ = alertPersistenceModule;
    voice_ = voiceModule;
    quiet_ = quietCoordinator;
    lastPersistenceSlot_ = -1;
    lastRenderedOwner_ = RenderOwner::Unknown;
    lastAlertGapRecoverMs_ = 0;
}

void DisplayPipelineModule::renderIdleOwner(uint32_t nowMs,
                                            const DisplayState& state,
                                            bool forceRedraw,
                                            bool restoreContext) {
    const V1Settings& s = settings_->get();
    const uint8_t persistSec = settings_->getSlotAlertPersistSec(s.activeSlot);

    if (s.activeSlot != lastPersistenceSlot_) {
        lastPersistenceSlot_ = s.activeSlot;
        alertPersistence_->clearPersistence();
    }

    if (persistSec > 0 && alertPersistence_->getPersistedAlert().isValid) {
        alertPersistence_->startPersistence(nowMs);

        const unsigned long persistMs = persistSec * 1000UL;
        if (alertPersistence_->shouldShowPersisted(nowMs, persistMs)) {
            if (forceRedraw || lastRenderedOwner_ != RenderOwner::Persisted) {
                display_->forceNextRedraw();
            }

            perfSetDisplayRenderScenario(restoreContext ? PerfDisplayRenderScenario::Restore
                                                        : PerfDisplayRenderScenario::Persisted);
            const unsigned long startUs = micros();
            display_->updatePersisted(alertPersistence_->getPersistedAlert(), state);
            const unsigned long endUs = micros();
            recordDisplayTiming("display.persisted", startUs, endUs);
            recordPerfTiming("display.persisted", startUs, endUs);
            perfClearDisplayRenderScenario();
            lastRenderedOwner_ = RenderOwner::Persisted;
            return;
        }

        PERF_INC(alertPersistExpires);
        alertPersistence_->clearPersistence();
    } else {
        alertPersistence_->clearPersistence();
    }

    if (forceRedraw || lastRenderedOwner_ != RenderOwner::Resting) {
        display_->forceNextRedraw();
    }

    perfSetDisplayRenderScenario(restoreContext ? PerfDisplayRenderScenario::Restore
                                                : PerfDisplayRenderScenario::Resting);
    const unsigned long startUs = micros();
    display_->update(state);
    const unsigned long endUs = micros();
    recordDisplayTiming("display.resting", startUs, endUs);
    recordPerfTiming("display.resting", startUs, endUs);
    perfClearDisplayRenderScenario();
    lastRenderedOwner_ = RenderOwner::Resting;
}

void DisplayPipelineModule::handleParsed(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ ||
        !voice_ || !displayMode_) {
        return;
    }

    DisplayState state = parser_->getDisplayState();
    const bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority =
        hasAlerts && parser_->getRenderablePriorityAlert(priority);
    if (hasRenderablePriority) {
        const AlertData rawPriority = parser_->getPriorityAlert();
        const bool rawRenderable = rawPriority.isValid &&
                                   rawPriority.band != BAND_NONE &&
                                   ((rawPriority.band == BAND_LASER) ||
                                    (rawPriority.frequency != 0));
        if (!rawRenderable) {
            PERF_INC(displayLiveFallbackToUsable);
        }
    }
    const V1Settings& settingsRef = settings_->get();

    if (!hasAlerts && state.activeBands != BAND_NONE) {
        const unsigned long gapNow = nowMs;
        if (gapNow - lastAlertGapRecoverMs_ > 50) {
            // Preserve partially assembled alert rows; parser freshness/timeout
            // guards handle stale data without discarding in-progress tables.
            const unsigned long gapStartUs = micros();
            ble_->requestAlertData();
            perfRecordDisplayGapRecoverUs(micros() - gapStartUs);
            lastAlertGapRecoverMs_ = gapNow;
        }
    }

    const unsigned long muteNow = nowMs;
    if (state.muted != debouncedMuteState_) {
        if (muteNow - lastMuteChangeMs_ > MUTE_DEBOUNCE_MS) {
            debouncedMuteState_ = state.muted;
            lastMuteChangeMs_ = muteNow;
        } else {
            state.muted = debouncedMuteState_;
        }
    }

    if (nowMs - lastDisplayDraw_ < DISPLAY_DRAW_MIN_MS) {
        PERF_INC(displaySkips);
        return;
    }
    lastDisplayDraw_ = nowMs;

    if (hasAlerts) {
        const int alertCount = parser_->getAlertCount();
        const auto& currentAlerts = parser_->getAllAlerts();
        const bool deferSecondaryCards = ble_->isConnectBurstSettling();
        const AlertData* renderAlerts = currentAlerts.data();
        int renderAlertCount = alertCount;
        AlertData priorityOnlyAlert[1];
        if (deferSecondaryCards && hasRenderablePriority) {
            priorityOnlyAlert[0] = priority;
            renderAlerts = priorityOnlyAlert;
            renderAlertCount = 1;
        }

        *displayMode_ = DisplayMode::LIVE;

        VoiceContext voiceCtx;
        voiceCtx.alerts = currentAlerts.data();
        voiceCtx.alertCount = alertCount;
        voiceCtx.priority = hasRenderablePriority ? &priority : nullptr;
        voiceCtx.isMuted = state.muted;
        voiceCtx.isProxyConnected = ble_->isProxyClientConnected();
        voiceCtx.mainVolume = state.mainVolume;
        voiceCtx.isSuppressed = false;
        voiceCtx.now = nowMs;

        if (quiet_) {
            quiet_->applyVoicePresentation(voiceCtx,
                                          speedMute_,
                                          hasRenderablePriority,
                                          hasRenderablePriority ? priority.band : BAND_NONE);
        }

        const unsigned long voiceStartUs = micros();
        const VoiceAction voiceAction = voice_->process(voiceCtx);
        perfRecordDisplayVoiceUs(micros() - voiceStartUs);

        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Live);
        const unsigned long startUs = micros();
        if (hasRenderablePriority) {
            display_->update(priority, renderAlerts, renderAlertCount, state);
        } else {
            display_->update(state);
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
            alertPersistence_->setPersistedAlert(priority);
        }
        lastRenderedOwner_ = RenderOwner::Live;
        return;
    }

    *displayMode_ = DisplayMode::IDLE;
    voice_->clearAllState();
    renderIdleOwner(nowMs, state, false, false);
}

void DisplayPipelineModule::restoreCurrentOwner(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ ||
        !voice_ || !displayMode_) {
        return;
    }

    display_->forceNextRedraw();

    if (!ble_->isConnected()) {
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        display_->showScanning();
        perfClearDisplayRenderScenario();
        *displayMode_ = DisplayMode::IDLE;
        lastRenderedOwner_ = RenderOwner::Scanning;
        return;
    }

    const DisplayState state = parser_->getDisplayState();
    const bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority =
        hasAlerts && parser_->getRenderablePriorityAlert(priority);

    if (hasAlerts) {
        *displayMode_ = DisplayMode::LIVE;
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        const unsigned long startUs = micros();
        if (hasRenderablePriority) {
            const auto& alerts = parser_->getAllAlerts();
            display_->update(priority, alerts.data(), parser_->getAlertCount(), state);
        } else {
            display_->update(state);
        }
        perfRecordDisplayScenarioRenderUs(micros() - startUs);
        perfClearDisplayRenderScenario();
        lastRenderedOwner_ = RenderOwner::Live;
        return;
    }

    *displayMode_ = DisplayMode::IDLE;
    renderIdleOwner(nowMs, state, true, true);
}

bool DisplayPipelineModule::allowsObdPairGesture(uint32_t nowMs) const {
    if (!displayMode_ || !parser_ || !settings_ || !alertPersistence_) {
        return false;
    }

    if (*displayMode_ != DisplayMode::IDLE) {
        return false;
    }

    if (parser_->hasAlerts()) {
        return false;
    }

    const V1Settings& s = settings_->get();
    const uint8_t persistSec = settings_->getSlotAlertPersistSec(s.activeSlot);
    if (persistSec > 0 &&
        alertPersistence_->getPersistedAlert().isValid &&
        alertPersistence_->shouldShowPersisted(nowMs, persistSec * 1000UL)) {
        return false;
    }

    return true;
}

void DisplayPipelineModule::recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    const unsigned long dur = endUs - startUs;
    displayLatencySum_ += dur;
    displayLatencyCount_++;
    if (dur > displayLatencyMax_) displayLatencyMax_ = dur;

    const unsigned long nowMs = millis();

    if ((nowMs - displayLatencyLastLog_) > DISPLAY_LOG_INTERVAL_MS && displayLatencyCount_ > 0) {
        displayLatencySum_ = 0;
        displayLatencyCount_ = 0;
        displayLatencyMax_ = 0;
        displayLatencyLastLog_ = nowMs;
    }
}

void DisplayPipelineModule::recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs) {
    const unsigned long dur = endUs - startUs;

    // Always record to perf metrics for scorecard attribution.
    perfRecordDisplayRenderUs(dur);
    perfRecordDisplayScenarioRenderUs(dur);
    PERF_INC(displayUpdates);

    if (!PERF_TIMING_LOGS) return;
    perfTimingAccum_ += dur;
    perfTimingCount_++;
    if (dur > perfTimingMax_) perfTimingMax_ = dur;
    const unsigned long nowMs = millis();
    if (nowMs - perfLastReport_ > 5000) {
        Serial.printf("[PERF] %s: avg=%luus max=%luus (n=%lu)\n",
                      label,
                      perfTimingAccum_ / perfTimingCount_,
                      perfTimingMax_,
                      perfTimingCount_);
        perfTimingAccum_ = 0;
        perfTimingCount_ = 0;
        perfTimingMax_ = 0;
        perfLastReport_ = nowMs;
    }
}
