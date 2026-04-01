#include "auto_push_module.h"

#include "perf_metrics.h"
#include "../quiet/quiet_coordinator_module.h"

#define AUTO_PUSH_LOGF(...) do { } while (0)
#define AUTO_PUSH_LOGLN(msg) do { } while (0)

void AutoPushModule::begin(SettingsManager* settingsMgr,
                           V1ProfileManager* profileMgr,
                           V1BLEClient* ble,
                           V1Display* disp,
                           QuietCoordinatorModule* quietCoordinator) {
    settings_ = settingsMgr;
    profiles_ = profileMgr;
    bleClient_ = ble;
    display_ = disp;
    quiet_ = quietCoordinator;
}

void AutoPushModule::armState(int slotIndex,
                              const AutoPushSlot& slot,
                              bool profileLoaded,
                              const V1Profile& profile,
                              bool isPushNow,
                              bool updateProfileIndicator) {
    state_ = State{};
    state_.slotIndex = slotIndex;
    state_.slot = slot;
    state_.profile = profileLoaded ? profile : V1Profile{};
    state_.profileLoaded = profileLoaded;
    state_.step = Step::WaitReady;
    state_.nextStepAtMs = millis() + 100;
    state_.isPushNow = isPushNow;

    if (display_ && updateProfileIndicator) {
        display_->drawProfileIndicator(slotIndex);
    }
}

AutoPushModule::QueueResult AutoPushModule::queuePreparedSlot(int slotIndex,
                                                              const AutoPushSlot& slot,
                                                              bool profileLoaded,
                                                              const V1Profile& profile,
                                                              bool isPushNow,
                                                              bool activateSlot,
                                                              bool countAutoPushStart,
                                                              bool updateProfileIndicator) {
    if (!settings_ || !profiles_ || !bleClient_ || !display_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    if (activateSlot) {
        settings_->setActiveSlot(clampedIndex);
    }
    if (countAutoPushStart) {
        PERF_INC(autoPushStarts);
    }

    armState(clampedIndex, slot, profileLoaded, profile, isPushNow, updateProfileIndicator);
    return QueueResult::QUEUED;
}

AutoPushModule::QueueResult AutoPushModule::queueSlotPush(int slotIndex,
                                                          bool activateSlot,
                                                          bool updateProfileIndicator) {
    if (!settings_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    const AutoPushSlot slot = settings_->getSlot(clampedIndex);
    return queuePreparedSlot(
        clampedIndex, slot, false, V1Profile{}, false, activateSlot, true, updateProfileIndicator);
}

AutoPushModule::QueueResult AutoPushModule::queuePushNow(const PushNowRequest& request) {
    if (!settings_ || !profiles_ || !bleClient_ || !display_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const int clampedIndex = std::max(0, std::min(2, request.slotIndex));
    AutoPushSlot slot = settings_->getSlot(clampedIndex);
    if (request.hasProfileOverride) {
        slot.profileName = request.profileName;
        slot.mode = request.hasModeOverride ? request.mode : V1_MODE_UNKNOWN;
    } else if (request.hasModeOverride) {
        slot.mode = request.mode;
    }

    if (slot.profileName.length() == 0) {
        return QueueResult::NO_PROFILE_CONFIGURED;
    }

    V1Profile profile;
    if (!profiles_->loadProfile(slot.profileName, profile)) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    AUTO_PUSH_LOGF("[PushNow] Queued slot=%d profile='%s' mode=%d\n",
                   clampedIndex,
                   slot.profileName.c_str(),
                   static_cast<int>(slot.mode));
    return queuePreparedSlot(
        clampedIndex, slot, true, profile, true, request.activateSlot, false, true);
}

void AutoPushModule::applySlotMuteToZero(V1UserSettings& userSettings, bool slotMuteToZero) {
    if (slotMuteToZero) {
        userSettings.bytes[0] &= ~0x10;
    } else {
        userSettings.bytes[0] |= 0x10;
    }
}

void AutoPushModule::process() {
    if (state_.step == Step::Idle) {
        return;
    }

    if (!bleClient_ || !bleClient_->isConnected()) {
        if (!state_.isPushNow) {
            PERF_INC(autoPushDisconnectAbort);
        }
        state_ = State{};
        return;
    }

    const unsigned long now = millis();
    if (now < state_.nextStepAtMs) {
        return;
    }

    auto schedulePushNowRetry = [&](const char* op) -> bool {
        if (!state_.isPushNow) {
            return false;
        }
        if (state_.commandRetries < kMaxPushNowCommandRetries) {
            state_.commandRetries++;
            PERF_INC(pushNowRetries);
            state_.nextStepAtMs = now + 30;
            AUTO_PUSH_LOGF("[PushNow] %s deferred, retry %u/%u\n",
                           op,
                           state_.commandRetries,
                           kMaxPushNowCommandRetries);
            return true;
        }

        PERF_INC(pushNowFailures);
        AUTO_PUSH_LOGF("[PushNow] ERROR: %s failed after %u retries\n",
                       op,
                       kMaxPushNowCommandRetries);
        state_ = State{};
        return true;
    };

    switch (state_.step) {
        case Step::WaitReady:
            state_.step = Step::Profile;
            state_.nextStepAtMs = now;
            return;

        case Step::Profile: {
            const AutoPushSlot& slot = state_.slot;
            if (!state_.profileLoaded) {
                if (slot.profileName.length() > 0) {
                    AUTO_PUSH_LOGF("[AutoPush] Loading profile: %s\n", slot.profileName.c_str());
                    V1Profile profile;
                    if (profiles_ && profiles_->loadProfile(slot.profileName, profile)) {
                        state_.profile = profile;
                        state_.profileLoaded = true;
                    } else {
                        if (!state_.isPushNow) {
                            PERF_INC(autoPushProfileLoadFail);
                        }
                        AUTO_PUSH_LOGF("[AutoPush] ERROR: Failed to load profile '%s'\n",
                                       slot.profileName.c_str());
                    }
                } else {
                    if (!state_.isPushNow) {
                        PERF_INC(autoPushNoProfile);
                    }
                    AUTO_PUSH_LOGLN("[AutoPush] No profile configured for active slot");
                }
            }

            if (state_.profileLoaded) {
                const bool slotMuteToZero = settings_->getSlotMuteToZero(state_.slotIndex);
                V1UserSettings modifiedSettings = state_.profile.settings;
                applySlotMuteToZero(modifiedSettings, slotMuteToZero);

                if (bleClient_->writeUserBytes(modifiedSettings.bytes)) {
                    bleClient_->startUserBytesVerification(modifiedSettings.bytes);
                    state_.profileWriteRetries = 0;
                    state_.commandRetries = 0;
                    state_.step = Step::ProfileReadback;
                    state_.nextStepAtMs = now + 30;
                    return;
                }

                if (!state_.isPushNow) {
                    PERF_INC(autoPushBusyRetries);
                }
                if (schedulePushNowRetry("writeUserBytes")) {
                    return;
                }

                if (state_.profileWriteRetries < kMaxProfileWriteRetries) {
                    state_.profileWriteRetries++;
                    AUTO_PUSH_LOGF("[AutoPush] Write busy, retrying (%u/%u)\n",
                                   state_.profileWriteRetries,
                                   kMaxProfileWriteRetries);
                    state_.step = Step::Profile;
                    state_.nextStepAtMs = now + 30;
                    return;
                }

                PERF_INC(autoPushProfileWriteFail);
                AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to push profile settings");
            }

            state_.commandRetries = 0;
            state_.step = Step::Display;
            state_.nextStepAtMs = now + 30;
            return;
        }

        case Step::ProfileReadback:
            bleClient_->requestUserBytes();
            state_.commandRetries = 0;
            state_.step = Step::Display;
            state_.nextStepAtMs = now + 30;
            return;

        case Step::Display: {
            const bool displayOn = !settings_->getSlotDarkMode(state_.slotIndex);
            if (!bleClient_->setDisplayOn(displayOn) && schedulePushNowRetry("setDisplayOn")) {
                return;
            }
            state_.commandRetries = 0;
            state_.step = Step::Mode;
            state_.nextStepAtMs = now + (state_.slot.mode != V1_MODE_UNKNOWN ? 30 : 0);
            return;
        }

        case Step::Mode: {
            if (state_.slot.mode != V1_MODE_UNKNOWN &&
                !bleClient_->setMode(static_cast<uint8_t>(state_.slot.mode))) {
                if (schedulePushNowRetry("setMode")) {
                    return;
                }
                if (!state_.isPushNow) {
                    PERF_INC(autoPushModeFail);
                }
            }

            state_.commandRetries = 0;
            const bool volumeChangeNeeded =
                (settings_->getSlotVolume(state_.slotIndex) != 0xFF ||
                 settings_->getSlotMuteVolume(state_.slotIndex) != 0xFF);
            state_.step = Step::Volume;
            state_.nextStepAtMs = now + (volumeChangeNeeded ? 30 : 0);
            return;
        }

        case Step::Volume: {
            const uint8_t mainVol = settings_->getSlotVolume(state_.slotIndex);
            const uint8_t muteVol = settings_->getSlotMuteVolume(state_.slotIndex);
            const bool volumeChangeNeeded = (mainVol != 0xFF || muteVol != 0xFF);
            const bool volumeSent =
                !volumeChangeNeeded ||
                (quiet_ && quiet_->sendVolume(QuietOwner::AutoPush, mainVol, muteVol));
            if (!volumeSent) {
                if (schedulePushNowRetry("setVolume")) {
                    return;
                }
                if (!state_.isPushNow) {
                    PERF_INC(autoPushVolumeFail);
                }
            }

            if (!state_.isPushNow) {
                PERF_INC(autoPushCompletes);
            }
            state_ = State{};
            return;
        }

        case Step::Idle:
        default:
            state_ = State{};
            return;
    }
}

String AutoPushModule::getStatusJson() const {
    const char* stepName = "Idle";
    switch (state_.step) {
        case Step::Idle: stepName = "Idle"; break;
        case Step::WaitReady: stepName = "WaitReady"; break;
        case Step::Profile: stepName = "Profile"; break;
        case Step::ProfileReadback: stepName = "ProfileReadback"; break;
        case Step::Display: stepName = "Display"; break;
        case Step::Mode: stepName = "Mode"; break;
        case Step::Volume: stepName = "Volume"; break;
    }

    const bool hasProfile = state_.slot.profileName.length() > 0;
    const char* profileName = hasProfile ? state_.slot.profileName.c_str() : "";

    char buf[192];
    snprintf(buf,
             sizeof(buf),
             "{\"active\":%s,\"slot\":%d,\"step\":\"%s\",\"profileLoaded\":%s,\"profileConfigured\":%s,\"profileName\":\"%s\"}",
             state_.step == Step::Idle ? "false" : "true",
             state_.slotIndex,
             stepName,
             state_.profileLoaded ? "true" : "false",
             hasProfile ? "true" : "false",
             profileName);
    return String(buf);
}
