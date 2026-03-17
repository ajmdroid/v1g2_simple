#include "auto_push_module.h"

#include "perf_metrics.h"

#define AUTO_PUSH_LOGF(...) do { } while (0)
#define AUTO_PUSH_LOGLN(msg) do { } while (0)

namespace {

const char* slotNameForIndex(int slotIndex) {
    static const char* kSlotNames[] = {"Default", "Highway", "Passenger Comfort"};
    return kSlotNames[std::max(0, std::min(2, slotIndex))];
}

}  // namespace

void AutoPushModule::begin(SettingsManager* settingsMgr,
                           V1ProfileManager* profileMgr,
                           V1BLEClient* ble,
                           V1Display* disp) {
    settings = settingsMgr;
    profiles = profileMgr;
    bleClient = ble;
    display = disp;
}

void AutoPushModule::armState(int slotIndex,
                              const AutoPushSlot& slot,
                              bool profileLoaded,
                              const V1Profile& profile,
                              bool isPushNow) {
    state = State{};
    state.slotIndex = slotIndex;
    state.slot = slot;
    state.profile = profileLoaded ? profile : V1Profile{};
    state.profileLoaded = profileLoaded;
    state.step = Step::WaitReady;
    state.nextStepAtMs = millis() + 100;
    state.isPushNow = isPushNow;

    if (display) {
        display->drawProfileIndicator(slotIndex);
    }
}

AutoPushModule::QueueResult AutoPushModule::queuePreparedSlot(int slotIndex,
                                                              const AutoPushSlot& slot,
                                                              bool profileLoaded,
                                                              const V1Profile& profile,
                                                              bool isPushNow,
                                                              bool activateSlot,
                                                              bool countAutoPushStart) {
    if (!settings || !profiles || !bleClient || !display) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    if (activateSlot) {
        settings->setActiveSlot(clampedIndex);
    }
    if (countAutoPushStart) {
        PERF_INC(autoPushStarts);
    }

    armState(clampedIndex, slot, profileLoaded, profile, isPushNow);
    return QueueResult::QUEUED;
}

AutoPushModule::QueueResult AutoPushModule::queueSlotPush(int slotIndex, bool activateSlot) {
    if (!settings) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    const AutoPushSlot slot = settings->getSlot(clampedIndex);
    return queuePreparedSlot(
        clampedIndex, slot, false, V1Profile{}, false, activateSlot, true);
}

void AutoPushModule::start(int slotIndex) {
    if (queueSlotPush(slotIndex) != QueueResult::QUEUED) {
        return;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    AUTO_PUSH_LOGF("[AutoPush] V1 connected - applying '%s' profile (slot %d)...\n",
                   slotNameForIndex(clampedIndex),
                   clampedIndex);
}

AutoPushModule::QueueResult AutoPushModule::queuePushNow(const PushNowRequest& request) {
    if (!settings || !profiles || !bleClient || !display) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const int clampedIndex = std::max(0, std::min(2, request.slotIndex));
    AutoPushSlot slot = settings->getSlot(clampedIndex);
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
    if (!profiles->loadProfile(slot.profileName, profile)) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    AUTO_PUSH_LOGF("[PushNow] Queued slot=%d profile='%s' mode=%d\n",
                   clampedIndex,
                   slot.profileName.c_str(),
                   static_cast<int>(slot.mode));
    return queuePreparedSlot(
        clampedIndex, slot, true, profile, true, request.activateSlot, false);
}

void AutoPushModule::applySlotMuteToZero(V1UserSettings& userSettings, bool slotMuteToZero) {
    if (slotMuteToZero) {
        userSettings.bytes[0] &= ~0x10;
    } else {
        userSettings.bytes[0] |= 0x10;
    }
}

void AutoPushModule::process() {
    if (state.step == Step::Idle) {
        return;
    }

    if (!bleClient || !bleClient->isConnected()) {
        if (!state.isPushNow) {
            PERF_INC(autoPushDisconnectAbort);
        }
        state = State{};
        return;
    }

    const unsigned long now = millis();
    if (now < state.nextStepAtMs) {
        return;
    }

    auto schedulePushNowRetry = [&](const char* op) -> bool {
        if (!state.isPushNow) {
            return false;
        }
        if (state.commandRetries < kMaxPushNowCommandRetries) {
            state.commandRetries++;
            PERF_INC(pushNowRetries);
            state.nextStepAtMs = now + 30;
            AUTO_PUSH_LOGF("[PushNow] %s deferred, retry %u/%u\n",
                           op,
                           state.commandRetries,
                           kMaxPushNowCommandRetries);
            return true;
        }

        PERF_INC(pushNowFailures);
        AUTO_PUSH_LOGF("[PushNow] ERROR: %s failed after %u retries\n",
                       op,
                       kMaxPushNowCommandRetries);
        state = State{};
        return true;
    };

    switch (state.step) {
        case Step::WaitReady:
            state.step = Step::Profile;
            state.nextStepAtMs = now;
            return;

        case Step::Profile: {
            const AutoPushSlot& slot = state.slot;
            if (!state.profileLoaded) {
                if (slot.profileName.length() > 0) {
                    AUTO_PUSH_LOGF("[AutoPush] Loading profile: %s\n", slot.profileName.c_str());
                    V1Profile profile;
                    if (profiles && profiles->loadProfile(slot.profileName, profile)) {
                        state.profile = profile;
                        state.profileLoaded = true;
                    } else {
                        if (!state.isPushNow) {
                            PERF_INC(autoPushProfileLoadFail);
                        }
                        AUTO_PUSH_LOGF("[AutoPush] ERROR: Failed to load profile '%s'\n",
                                       slot.profileName.c_str());
                    }
                } else {
                    if (!state.isPushNow) {
                        PERF_INC(autoPushNoProfile);
                    }
                    AUTO_PUSH_LOGLN("[AutoPush] No profile configured for active slot");
                }
            }

            if (state.profileLoaded) {
                const bool slotMuteToZero = settings->getSlotMuteToZero(state.slotIndex);
                V1UserSettings modifiedSettings = state.profile.settings;
                applySlotMuteToZero(modifiedSettings, slotMuteToZero);

                if (bleClient->writeUserBytes(modifiedSettings.bytes)) {
                    bleClient->startUserBytesVerification(modifiedSettings.bytes);
                    state.profileWriteRetries = 0;
                    state.commandRetries = 0;
                    state.step = Step::ProfileReadback;
                    state.nextStepAtMs = now + 30;
                    return;
                }

                if (!state.isPushNow) {
                    PERF_INC(autoPushBusyRetries);
                }
                if (schedulePushNowRetry("writeUserBytes")) {
                    return;
                }

                if (state.profileWriteRetries < kMaxProfileWriteRetries) {
                    state.profileWriteRetries++;
                    AUTO_PUSH_LOGF("[AutoPush] Write busy, retrying (%u/%u)\n",
                                   state.profileWriteRetries,
                                   kMaxProfileWriteRetries);
                    state.step = Step::Profile;
                    state.nextStepAtMs = now + 30;
                    return;
                }

                PERF_INC(autoPushProfileWriteFail);
                AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to push profile settings");
            }

            state.commandRetries = 0;
            state.step = Step::Display;
            state.nextStepAtMs = now + 30;
            return;
        }

        case Step::ProfileReadback:
            bleClient->requestUserBytes();
            state.commandRetries = 0;
            state.step = Step::Display;
            state.nextStepAtMs = now + 30;
            return;

        case Step::Display: {
            const bool displayOn = !settings->getSlotDarkMode(state.slotIndex);
            if (!bleClient->setDisplayOn(displayOn) && schedulePushNowRetry("setDisplayOn")) {
                return;
            }
            state.commandRetries = 0;
            state.step = Step::Mode;
            state.nextStepAtMs = now + (state.slot.mode != V1_MODE_UNKNOWN ? 30 : 0);
            return;
        }

        case Step::Mode: {
            if (state.slot.mode != V1_MODE_UNKNOWN &&
                !bleClient->setMode(static_cast<uint8_t>(state.slot.mode))) {
                if (schedulePushNowRetry("setMode")) {
                    return;
                }
                if (!state.isPushNow) {
                    PERF_INC(autoPushModeFail);
                }
            }

            state.commandRetries = 0;
            const bool volumeChangeNeeded =
                (settings->getSlotVolume(state.slotIndex) != 0xFF ||
                 settings->getSlotMuteVolume(state.slotIndex) != 0xFF);
            state.step = Step::Volume;
            state.nextStepAtMs = now + (volumeChangeNeeded ? 30 : 0);
            return;
        }

        case Step::Volume: {
            const uint8_t mainVol = settings->getSlotVolume(state.slotIndex);
            const uint8_t muteVol = settings->getSlotMuteVolume(state.slotIndex);
            if ((mainVol != 0xFF || muteVol != 0xFF) &&
                !bleClient->setVolume(mainVol, muteVol)) {
                if (schedulePushNowRetry("setVolume")) {
                    return;
                }
                if (!state.isPushNow) {
                    PERF_INC(autoPushVolumeFail);
                }
            }

            if (!state.isPushNow) {
                PERF_INC(autoPushCompletes);
            }
            state = State{};
            return;
        }

        case Step::Idle:
        default:
            state = State{};
            return;
    }
}

String AutoPushModule::getStatusJson() const {
    const char* stepName = "Idle";
    switch (state.step) {
        case Step::Idle: stepName = "Idle"; break;
        case Step::WaitReady: stepName = "WaitReady"; break;
        case Step::Profile: stepName = "Profile"; break;
        case Step::ProfileReadback: stepName = "ProfileReadback"; break;
        case Step::Display: stepName = "Display"; break;
        case Step::Mode: stepName = "Mode"; break;
        case Step::Volume: stepName = "Volume"; break;
    }

    const bool hasProfile = state.slot.profileName.length() > 0;
    const char* profileName = hasProfile ? state.slot.profileName.c_str() : "";

    char buf[192];
    snprintf(buf,
             sizeof(buf),
             "{\"active\":%s,\"slot\":%d,\"step\":\"%s\",\"profileLoaded\":%s,\"profileConfigured\":%s,\"profileName\":\"%s\"}",
             state.step == Step::Idle ? "false" : "true",
             state.slotIndex,
             stepName,
             state.profileLoaded ? "true" : "false",
             hasProfile ? "true" : "false",
             profileName);
    return String(buf);
}
