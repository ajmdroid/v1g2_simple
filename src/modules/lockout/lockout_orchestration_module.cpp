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
#include "../quiet/quiet_coordinator_module.h"
#include "../speed/speed_source_selector.h"
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
                                       SystemEventBus* eventBus,
                                       PerfCounters* perfCounters,
                                       TimeService* timeSvc,
                                       QuietCoordinatorModule* quietCoordinator) {
    ble_ = ble;
    parser_ = parser;
    settings_ = settings;
    display_ = display;
    enforcer_ = enforcer;
    index_ = index;
    sigCapture_ = sigCapture;
    eventBus_ = eventBus;
    perfCounters_ = perfCounters;
    timeSvc_ = timeSvc;
    quiet_ = quietCoordinator;
}

LockoutOrchestrationResult LockoutOrchestrationModule::process(
        uint32_t nowMs,
        const GpsRuntimeStatus& gpsStatus,
        bool proxyClientConnected,
        bool enableSignalTrace) {

    LockoutOrchestrationResult result;
    if (!ble_ || !parser_ || !settings_ || !display_ || !enforcer_ ||
        !index_ || !sigCapture_ || !eventBus_ || !perfCounters_) {
        return result;
    }

    const SpeedSelection selectedSpeed = speedSourceSelector.selectedSpeed();
    const int64_t nowEpochMs = timeSvc_ ? timeSvc_->nowEpochMsOr0() : 0;
    const int32_t tzOffsetMinutes = timeSvc_ ? timeSvc_->tzOffsetMinutes() : 0;

    sigCapture_->capturePriorityObservation(
        nowMs,
        *parser_,
        gpsStatus,
        selectedSpeed,
        enableSignalTrace);

    if (!proxyClientConnected) {
        enforcer_->process(nowMs,
                           nowEpochMs,
                           tzOffsetMinutes,
                           *parser_,
                           gpsStatus);

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

        if (quiet_) {
            quiet_->processLockoutMute(lockRes,
                                       lockoutGuard,
                                       ble_->isConnected(),
                                       lockoutDisplayState.muted,
                                       overrideBandActive,
                                       nowMs);
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
                    gpsStatus.courseValid &&
                        gpsStatus.courseAgeMs <= LOCKOUT_GPS_COURSE_MAX_AGE_MS,
                    gpsStatus.courseDeg,
                    bufferE5, nearbyBuf, 16);
            }
            // Use external volume hint (e.g. from speed volume) for capture
            // so pre-quiet saves the true user volume, not a lowered one.
            const uint8_t pqMainVol = (volumeHintMain_ != 0xFF)
                                        ? volumeHintMain_ : pqState.mainVolume;
            const PreQuietDecision pqDecision = evaluatePreQuiet(
                lockoutSettings.gpsLockoutPreQuiet,
                enforceMode,
                ble_->isConnected(),
                gpsStatus.locationValid,
                parser_->hasAlerts(),
                lockRes.evaluated,
                effectiveLockoutMute,
                nearbyCount,
                pqMainVol,
                pqState.muteVolume,
                nowMs,
                preQuietState_,
                overrideBandActive);
            if (pqDecision.action == PreQuietDecision::DROP_VOLUME) {
                result.volumeCommand.type = LockoutVolumeCommandType::PreQuietDrop;
                result.volumeCommand.volume = pqDecision.volume;
                result.volumeCommand.muteVolume = pqDecision.muteVolume;
            } else if (pqDecision.action == PreQuietDecision::RESTORE_VOLUME) {
                result.volumeCommand.type = LockoutVolumeCommandType::PreQuietRestore;
                result.volumeCommand.volume = pqDecision.volume;
                result.volumeCommand.muteVolume = pqDecision.muteVolume;
            }
            const bool preQuietActive = preQuietState_.phase == PreQuietPhase::DROPPED;
            if (quiet_) {
                quiet_->setPreQuietActive(preQuietActive);
            }
        }
    } else {
        // Proxy-connected sessions are display-first:
        // keep learner capture active, but disable runtime lockout enforcement.
        display_->setLockoutIndicator(false);
        reset();
    }

    return result;
}

void LockoutOrchestrationModule::reset() {
    preQuietState_ = PreQuietState{};
    volumeHintMain_ = 0xFF;
    volumeHintMute_ = 0;
    if (quiet_) {
        quiet_->resetLockoutChannel();
        quiet_->setPreQuietActive(false);
    }
}
