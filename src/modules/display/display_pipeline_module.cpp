#include "display_pipeline_module.h"
#include "display.h"
#include "packet_parser.h"
#include "display_mode.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/voice/voice_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/quiet/quiet_coordinator_voice_templates.h"
#include "modules/alp/alp_runtime_module.h"
#include "perf_metrics.h"
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
            if (forceRedraw) {
                display_->forceNextRedraw();
            }

            perfSetDisplayRenderScenario(restoreContext ? PerfDisplayRenderScenario::Restore
                                                        : PerfDisplayRenderScenario::Persisted);
            const unsigned long startUs = micros();
            display_->updatePersisted(alertPersistence_->getPersistedAlert(), state);
            const unsigned long endUs = micros();
            recordPerfTiming(startUs, endUs);
            perfClearDisplayRenderScenario();
            return;
        }

        PERF_INC(alertPersistExpires);
        alertPersistence_->clearPersistence();
    } else {
        alertPersistence_->clearPersistence();
    }

    if (forceRedraw) {
        display_->forceNextRedraw();
    }

    perfSetDisplayRenderScenario(restoreContext ? PerfDisplayRenderScenario::Restore
                                                : PerfDisplayRenderScenario::Resting);
    const unsigned long startUs = micros();
    display_->update(state);
    const unsigned long endUs = micros();
    recordPerfTiming(startUs, endUs);
    perfClearDisplayRenderScenario();
}

void DisplayPipelineModule::handleParsed(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ ||
        !voice_ || !displayMode_) {
        return;
    }

    DisplayState state = parser_->getDisplayState();
    bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    bool hasRenderablePriority =
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

    // ── ALP frequency-area override ────────────────────────────────────
    // V1-shape projection: ask the ALP module two questions —
    // "is there a laser event to show?" and "which gun?" — and render
    // from those. All state-machine / self-test / TEARDOWN details stay
    // inside the ALP module. ALP laser overrides everything — even Ka.
    const AlpGunType alpGun = alp_ ? alp_->eventGun() : AlpGunType::UNKNOWN;
    if (alp_ && alp_->hasLaserEvent() && alpGun != AlpGunType::UNKNOWN) {
        display_->setAlpFrequencyOverride(alpGunAbbrev(alpGun));
    } else {
        display_->clearAlpFrequencyOverride();
    }
    // Synthetic alert injection into the V1 LIVE pipeline — only during
    // active detection (excludes post-alert TEARDOWN rescan window).
    const bool alpActive = alp_ && alp_->isLaserDetecting();

    // ── V1 laser suppression when ALP owns the laser display ───────────
    // When the ALP is connected and producing data, it owns the laser
    // channel end-to-end — its own hardware renders direction (RED/YELLOW
    // LEDs) and fires audio via its buzzer, and the V1 Gen2 unit itself
    // does the same on its speaker. v1_simple doesn't need to duplicate
    // the laser alert. Filtering here eliminates the "ghost LASER tail"
    // symptom where V1's alert-persistence held a duplicate laser alert
    // visible on the display after the ALP engagement closed.
    //
    // Fallback: when ALP drifts to IDLE (UART timeout) or is disabled,
    // ownsLaserDisplay() returns false and V1 laser alerts pass through
    // normally as a backup detection channel.
    const bool alpOwnsLaser = alp_ && alp_->ownsLaserDisplay();
    std::array<AlertData, PacketParser::MAX_ALERTS> filteredAlerts;
    int filteredAlertCount = 0;
    if (alpOwnsLaser) {
        state.activeBands &= ~BAND_LASER;
        if (hasAlerts) {
            const auto& raw = parser_->getAllAlerts();
            const int rawCount = parser_->getAlertCount();
            for (int i = 0; i < rawCount; ++i) {
                if (raw[i].band != BAND_LASER) {
                    filteredAlerts[filteredAlertCount++] = raw[i];
                }
            }
            hasAlerts = (filteredAlertCount > 0);
        }
        if (hasRenderablePriority && priority.band == BAND_LASER) {
            // Priority was laser — drop it. Take the first non-laser
            // alert from the filtered list as the new priority. If
            // nothing remains, there is no renderable priority.
            hasRenderablePriority = false;
            for (int i = 0; i < filteredAlertCount; ++i) {
                if (filteredAlerts[i].isValid &&
                    filteredAlerts[i].band != BAND_NONE &&
                    filteredAlerts[i].frequency != 0) {
                    priority = filteredAlerts[i];
                    hasRenderablePriority = true;
                    break;
                }
            }
        }
    }

    // No mute debounce — trust the parser's muted state directly.
    // No display throttle — element caches handle SPI saturation.
    // No gap recovery — parser freshness/timeout guards handle stale data.

    if (hasAlerts) {
        const int alertCount = alpOwnsLaser ? filteredAlertCount
                                             : parser_->getAlertCount();
        const AlertData* allAlertsData = alpOwnsLaser
                                          ? filteredAlerts.data()
                                          : parser_->getAllAlerts().data();
        const bool deferSecondaryCards = ble_->isConnectBurstSettling();
        const AlertData* renderAlerts = allAlertsData;
        int renderAlertCount = alertCount;
        AlertData priorityOnlyAlert[1];
        if (deferSecondaryCards && hasRenderablePriority) {
            priorityOnlyAlert[0] = priority;
            renderAlerts = priorityOnlyAlert;
            renderAlertCount = 1;
        }

        *displayMode_ = DisplayMode::LIVE;

        VoiceContext voiceCtx;
        voiceCtx.alerts = allAlertsData;
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
        recordPerfTiming(startUs, endUs);
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
        return;
    }

    // ALP active with no V1 alerts — synthesize a laser alert display.
    // The frequency override is already set above; we just need to push
    // the display into live mode with a synthetic laser priority alert.
    if (alpActive) {
        AlertData alpAlert;
        alpAlert.isValid = true;
        alpAlert.band = BAND_LASER;
        alpAlert.frequency = 0;   // Laser has no frequency — gun abbrev is in override
        alpAlert.direction = DIR_FRONT;  // Default ahead until we have ALP direction data
        alpAlert.frontStrength = 8;      // Max signal — ALP is local detection

        *displayMode_ = DisplayMode::LIVE;

        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Live);
        const unsigned long startUs = micros();
        display_->update(alpAlert, &alpAlert, 1, state);
        const unsigned long endUs = micros();
        recordPerfTiming(startUs, endUs);
        perfClearDisplayRenderScenario();
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
        return;
    }

    DisplayState state = parser_->getDisplayState();
    bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    bool hasRenderablePriority =
        hasAlerts && parser_->getRenderablePriorityAlert(priority);

    // Restore ALP override state (same logic as handleParsed — V1-shape
    // projection via eventGun()/hasLaserEvent()/isLaserDetecting()).
    const AlpGunType alpGun = alp_ ? alp_->eventGun() : AlpGunType::UNKNOWN;
    if (alp_ && alp_->hasLaserEvent() && alpGun != AlpGunType::UNKNOWN) {
        display_->setAlpFrequencyOverride(alpGunAbbrev(alpGun));
    } else {
        display_->clearAlpFrequencyOverride();
    }
    const bool alpActive = alp_ && alp_->isLaserDetecting();

    // V1 laser suppression when ALP owns the laser display — mirrors the
    // filter in handleParsed(). See comment there for full rationale.
    const bool alpOwnsLaser = alp_ && alp_->ownsLaserDisplay();
    std::array<AlertData, PacketParser::MAX_ALERTS> filteredAlerts;
    int filteredAlertCount = 0;
    if (alpOwnsLaser) {
        state.activeBands &= ~BAND_LASER;
        if (hasAlerts) {
            const auto& raw = parser_->getAllAlerts();
            const int rawCount = parser_->getAlertCount();
            for (int i = 0; i < rawCount; ++i) {
                if (raw[i].band != BAND_LASER) {
                    filteredAlerts[filteredAlertCount++] = raw[i];
                }
            }
            hasAlerts = (filteredAlertCount > 0);
        }
        if (hasRenderablePriority && priority.band == BAND_LASER) {
            hasRenderablePriority = false;
            for (int i = 0; i < filteredAlertCount; ++i) {
                if (filteredAlerts[i].isValid &&
                    filteredAlerts[i].band != BAND_NONE &&
                    filteredAlerts[i].frequency != 0) {
                    priority = filteredAlerts[i];
                    hasRenderablePriority = true;
                    break;
                }
            }
        }
    }

    if (hasAlerts) {
        *displayMode_ = DisplayMode::LIVE;
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        const unsigned long startUs = micros();
        if (hasRenderablePriority) {
            const AlertData* alertsData = alpOwnsLaser
                                           ? filteredAlerts.data()
                                           : parser_->getAllAlerts().data();
            const int alertCount = alpOwnsLaser ? filteredAlertCount
                                                 : parser_->getAlertCount();
            display_->update(priority, alertsData, alertCount, state);
        } else {
            display_->update(state);
        }
        perfRecordDisplayScenarioRenderUs(micros() - startUs);
        perfClearDisplayRenderScenario();
        return;
    }

    // ALP active with no V1 alerts — show synthetic laser alert
    if (alpActive) {
        AlertData alpAlert;
        alpAlert.isValid = true;
        alpAlert.band = BAND_LASER;
        alpAlert.frequency = 0;
        alpAlert.direction = DIR_FRONT;
        alpAlert.frontStrength = 8;

        *displayMode_ = DisplayMode::LIVE;
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        const unsigned long startUs = micros();
        display_->update(alpAlert, &alpAlert, 1, state);
        perfRecordDisplayScenarioRenderUs(micros() - startUs);
        perfClearDisplayRenderScenario();
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

void DisplayPipelineModule::recordPerfTiming(unsigned long startUs, unsigned long endUs) {
    const unsigned long dur = endUs - startUs;
    perfRecordDisplayRenderUs(dur);
    perfRecordDisplayScenarioRenderUs(dur);
    PERF_INC(displayUpdates);
}
