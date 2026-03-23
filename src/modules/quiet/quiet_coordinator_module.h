#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "../lockout/lockout_runtime_mute_controller.h"

class V1BLEClient;
class PacketParser;
struct LockoutVolumeCommand;
struct VoiceContext;

enum class QuietOwner : uint8_t {
    None = 0,
    LockoutMute,
    LockoutOverride,
    PreQuiet,
    SpeedVolume,
    VolumeFade,
    TapGesture,
    WifiCommand,
    AutoPush,
};

const char* quietOwnerName(QuietOwner owner);

struct QuietIntent {
    QuietOwner owner = QuietOwner::None;
    bool hasMute = false;
    bool mute = false;
    bool hasVolume = false;
    uint8_t volume = 0xFF;
    uint8_t muteVolume = 0;
};

struct QuietDesiredState {
    QuietOwner muteOwner = QuietOwner::None;
    bool mutePending = false;
    bool mute = false;

    QuietOwner volumeOwner = QuietOwner::None;
    bool volumePending = false;
    uint8_t volume = 0xFF;
    uint8_t muteVolume = 0;
};

struct QuietCommittedState {
    bool connected = false;
    bool hasDisplayState = false;
    bool muted = false;
    uint8_t mainVolume = 0;
    uint8_t muteVolume = 0;
};

struct QuietPresentationState {
    QuietOwner activeMuteOwner = QuietOwner::None;
    QuietOwner activeVolumeOwner = QuietOwner::None;
    bool preQuietActive = false;
    bool speedVolZeroActive = false;
    bool voiceSuppressed = false;
    bool voiceAllowVolZeroBypass = false;
    bool effectiveMuted = false;
};

class QuietCoordinatorModule {
public:
    void begin(V1BLEClient* bleClient, PacketParser* parser);

    void reset();
    void resetLockoutChannel();

    bool sendMute(QuietOwner owner, bool muted);
    bool sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume);

    bool processLockoutMute(const LockoutEnforcerResult& lockRes,
                            const GpsLockoutCoreGuardStatus& lockoutGuard,
                            bool bleConnected,
                            bool v1Muted,
                            bool overrideBandActive,
                            uint32_t nowMs);

    template <typename VolumeFadeLike>
    bool handleLockoutVolumeCommand(const LockoutVolumeCommand& command,
                                    uint32_t nowMs,
                                    VolumeFadeLike* volumeFade);
    bool retryPendingPreQuietRestore(uint32_t nowMs);
    bool hasPendingPreQuietRestore() const { return pendingPqRestoreVol_ != 0xFF; }

    template <typename SpeedMuteLike, typename LockoutLike, typename VolumeFadeLike>
    bool processSpeedVolume(uint32_t nowMs,
                            const SpeedMuteLike& speedMute,
                            LockoutLike* lockout,
                            VolumeFadeLike* volumeFade);
    bool retryPendingSpeedVolRestore(uint32_t nowMs);
    bool isSpeedVolumeActive() const { return speedVolActive_; }

    template <typename VolumeFadeLike>
    bool executeVolumeFade(uint32_t nowMs,
                           bool lockoutPrioritySuppressed,
                           VolumeFadeLike* volumeFade);

    void setPreQuietActive(bool active);

    template <typename SpeedMuteLike>
    void applyVoicePresentation(VoiceContext& voiceCtx,
                                const SpeedMuteLike* speedMute,
                                bool prioritySuppressed,
                                bool hasRenderablePriority,
                                uint8_t priorityBand);

    const QuietDesiredState& getDesiredState() const { return desired_; }
    QuietCommittedState getCommittedState();
    const QuietPresentationState& getPresentationState() const { return presentation_; }

private:
    void syncCommittedState();
    void refreshPendingState();

    template <typename SpeedMuteLike>
    void updateSpeedVolPresentation(const SpeedMuteLike* speedMute);

    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;

    QuietDesiredState desired_{};
    QuietCommittedState committed_{};
    QuietPresentationState presentation_{};

    LockoutRuntimeMuteState lockoutMuteState_{};
    bool overrideUnmuteActive_ = false;
    uint32_t overrideUnmuteLastRetryMs_ = 0;
    uint8_t overrideUnmuteRetryCount_ = 0;
    static constexpr uint8_t MAX_OVERRIDE_UNMUTE_RETRIES = 15;
    static constexpr uint32_t OVERRIDE_UNMUTE_RETRY_MS = 400;

    uint8_t pendingPqRestoreVol_ = 0xFF;
    uint8_t pendingPqRestoreMuteVol_ = 0;
    uint32_t pendingPqRestoreSetMs_ = 0;
    uint32_t pendingPqRestoreLastRetryMs_ = 0;
    static constexpr uint32_t PQ_RESTORE_TIMEOUT_MS = 2000;
    static constexpr uint32_t PQ_RESTORE_RETRY_INTERVAL_MS = 75;

    bool speedVolActive_ = false;
    uint8_t speedVolSavedOriginal_ = 0xFF;
    uint8_t speedVolSavedMuteVol_ = 0;
    uint8_t pendingSpeedVolRestoreVol_ = 0xFF;
    uint8_t pendingSpeedVolRestoreMuteVol_ = 0;
    uint32_t pendingSpeedVolRestoreSetMs_ = 0;
    uint32_t pendingSpeedVolRestoreLastRetryMs_ = 0;
    uint32_t speedVolLastRetryMs_ = 0;
    static constexpr uint32_t SPEED_VOL_RETRY_INTERVAL_MS = 75;
    static constexpr uint32_t SPEED_VOL_RESTORE_TIMEOUT_MS = 2000;
};
