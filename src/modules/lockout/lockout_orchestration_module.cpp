#include "lockout_orchestration_module.h"

#include <Arduino.h>
#include <cmath>

#include "lockout_enforcer.h"
#include "lockout_index.h"
#include "signal_capture_module.h"
#include "lockout_runtime_mute_controller.h"
#include "lockout_pre_quiet_controller.h"
#include "../gps/gps_lockout_safety.h"
#include "../gps/gps_runtime_module.h"
#include "../volume_fade/volume_fade_module.h"
#include "../system/system_event_bus.h"
#include "../../packet_parser.h"
#include "../../ble_client.h"
#include "../../settings.h"
#include "../../display.h"
#include "../../perf_metrics.h"
#include "../../time_service.h"

void LockoutOrchestrationModule::begin(V1BLEClient* ble,
                                       PacketParser* parser,
                                       SettingsManager* settings,
                                       V1Display* display,
                                       LockoutEnforcer* enforcer,
                                       LockoutIndex* index,
                                       SignalCaptureModule* sigCapture,
                                       VolumeFadeModule* volFade,
                                       SystemEventBus* eventBus,
                                       PerfCounters* perfCounters,
                                       TimeService* timeSvc) {
    ble_ = ble;
    parser_ = parser;
    settings_ = settings;
    display_ = display;
    enforcer_ = enforcer;
    index_ = index;
    sigCapture_ = sigCapture;
    volFade_ = volFade;
    eventBus_ = eventBus;
    perfCounters_ = perfCounters;
    timeSvc_ = timeSvc;
}

LockoutOrchestrationResult LockoutOrchestrationModule::process(
        uint32_t nowMs,
        const GpsRuntimeStatus& gpsStatus,
        bool proxyClientConnected,
        bool enableSignalTrace) {

    LockoutOrchestrationResult result;

    sigCapture_->capturePriorityObservation(
        nowMs,
        *parser_,
        gpsStatus,
        enableSignalTrace);

    if (!proxyClientConnected) {
        enforcer_->process(nowMs, timeSvc_->nowEpochMsOr0(), *parser_, gpsStatus);

        const auto& lockRes = enforcer_->lastResult();
        const DisplayState& lockoutDisplayState = parser_->getDisplayState();
        constexpr uint8_t kMuteOverrideBandMask = static_cast<uint8_t>(BAND_LASER | BAND_KA);
        const bool overrideBandActive =
            (lockoutDisplayState.activeBands & kMuteOverrideBandMask) != 0;

        // ENFORCE mute execution: send mute to V1 when lockout decides to suppress.
        // Rate-limited: only send once per lockout-match cycle (not every frame).
        const V1Settings& lockoutSettings = settings_->get();
        const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
            lockoutSettings.gpsLockoutCoreGuardEnabled,
            lockoutSettings.gpsLockoutMaxQueueDrops,
            lockoutSettings.gpsLockoutMaxPerfDrops,
            lockoutSettings.gpsLockoutMaxEventBusDrops,
            perfCounters_->queueDrops.load(),
            perfCounters_->perfDrop.load(),
            eventBus_->getDropCount());

        const bool enforceMode =
            lockRes.mode == static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);
        const bool enforceAllowed = enforceMode && !lockoutGuard.tripped;
        const bool effectiveLockoutMute =
            lockRes.evaluated && lockRes.shouldMute && enforceAllowed && !overrideBandActive;

        // Suppress local priority announcements in ENFORCE lockout matches.
        result.prioritySuppressed = effectiveLockoutMute;

        // Feed lockout decision into display indicator before rendering.
        display_->setLockoutIndicator(effectiveLockoutMute);

        const LockoutRuntimeMuteDecision muteDecision =
            evaluateLockoutRuntimeMute(lockRes,
                                      lockoutGuard,
                                      ble_->isConnected(),
                                      lockoutDisplayState.muted,
                                      overrideBandActive,
                                      muteState_);

        if (muteDecision.sendMute) {
            ble_->setMute(true);
            Serial.println("[Lockout] ENFORCE: mute sent to V1");
        }
        if (muteDecision.sendUnmute) {
            ble_->setMute(false);
            Serial.println("[Lockout] ENFORCE: unmute sent to V1");
        }
        if (muteDecision.logGuardBlocked) {
            Serial.printf("[Lockout] ENFORCE blocked by core guard (%s)\n", lockoutGuard.reason);
        }

        // Safety override: live Ka/Laser must break through mute.
        // Retry at a low rate until V1 reports unmuted, capped to avoid
        // flooding a stuck/noisy link forever.
        constexpr uint32_t OVERRIDE_UNMUTE_RETRY_MS = 400;
        const bool needsOverrideUnmute =
            ble_->isConnected() && overrideBandActive && lockoutDisplayState.muted;
        if (needsOverrideUnmute) {
            if (overrideUnmuteRetryCount_ >= MAX_OVERRIDE_UNMUTE_RETRIES) {
                // Exhausted — stop retrying until condition recycles.
                if (overrideUnmuteActive_) {
                    Serial.printf("[Safety] Override unmute exhausted after %u retries\n",
                                  overrideUnmuteRetryCount_);
                    overrideUnmuteActive_ = false;
                }
            } else if (!overrideUnmuteActive_ ||
                static_cast<uint32_t>(nowMs - overrideUnmuteLastRetryMs_) >= OVERRIDE_UNMUTE_RETRY_MS) {
                ble_->setMute(false);
                overrideUnmuteLastRetryMs_ = nowMs;
                overrideUnmuteRetryCount_++;
                if (!overrideUnmuteActive_) {
                    Serial.println("[Safety] Ka/Laser override active: unmute sent to V1");
                }
                overrideUnmuteActive_ = true;
            }
        } else {
            overrideUnmuteActive_ = false;
            overrideUnmuteLastRetryMs_ = 0;
            overrideUnmuteRetryCount_ = 0;
        }

        // Pre-quiet: proactively drop volume when GPS is in a lockout zone.
        // findNearbyDirectional is position-only, O(N) — same scan the enforcer
        // uses.  When preQuietBufferE5 > 0, zones with a heading get an
        // asymmetric buffer: inflated on the approach side (enter mute early)
        // and base-radius-only on the departure side (release once past).
        // Omni-directional zones and missing course data fall back to
        // symmetric inflation.
        {
            const DisplayState& pqState = parser_->getDisplayState();
            size_t nearbyCount = 0;
            if (gpsStatus.locationValid &&
                std::isfinite(gpsStatus.latitudeDeg) &&
                std::isfinite(gpsStatus.longitudeDeg)) {
                const int32_t latE5 = static_cast<int32_t>(lroundf(gpsStatus.latitudeDeg * 100000.0f));
                const int32_t lonE5 = static_cast<int32_t>(lroundf(gpsStatus.longitudeDeg * 100000.0f));
                int16_t nearbyBuf[16];
                const uint16_t bufferE5 = lockoutSettings.gpsLockoutPreQuietBufferE5;
                nearbyCount = index_->findNearbyDirectional(
                    latE5, lonE5,
                    gpsStatus.courseValid, gpsStatus.courseDeg,
                    bufferE5, nearbyBuf, 16);
            }
            const PreQuietDecision pqDecision = evaluatePreQuiet(
                lockoutSettings.gpsLockoutPreQuiet,
                enforceMode,
                ble_->isConnected(),
                gpsStatus.locationValid,
                parser_->hasAlerts(),
                lockRes.evaluated,
                effectiveLockoutMute,
                nearbyCount,
                pqState.mainVolume,
                pqState.muteVolume,
                nowMs,
                preQuietState_,
                overrideBandActive);
            if (pqDecision.action == PreQuietDecision::DROP_VOLUME) {
                ble_->setVolume(pqDecision.volume, pqDecision.muteVolume);
                Serial.println("[Lockout] PRE-QUIET: volume dropped in lockout zone");
            } else if (pqDecision.action == PreQuietDecision::RESTORE_VOLUME) {
                ble_->setVolume(pqDecision.volume, pqDecision.muteVolume);
                // Tell VolumeFade the real baseline so it doesn't capture stale echo.
                volFade_->setBaselineHint(pqDecision.volume, pqDecision.muteVolume, nowMs);
                Serial.println("[Lockout] PRE-QUIET: volume restored");
            }
            display_->setPreQuietActive(preQuietState_.phase == PreQuietPhase::DROPPED);
        }
    } else {
        // Proxy-connected sessions are display-first:
        // keep learner capture active, but disable runtime lockout enforcement.
        display_->setLockoutIndicator(false);
        display_->setPreQuietActive(false);
        reset();
    }

    return result;
}

void LockoutOrchestrationModule::reset() {
    muteState_ = LockoutRuntimeMuteState{};
    preQuietState_ = PreQuietState{};
    overrideUnmuteActive_ = false;
    overrideUnmuteLastRetryMs_ = 0;
    overrideUnmuteRetryCount_ = 0;
}
