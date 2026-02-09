#include "auto_push_module.h"

#define AUTO_PUSH_LOGF(...) do { if (kDebugLogs) Serial.printf(__VA_ARGS__); } while (0)
#define AUTO_PUSH_LOGLN(msg) do { if (kDebugLogs) Serial.println(msg); } while (0)

void AutoPushModule::begin(SettingsManager* settingsMgr,
               V1ProfileManager* profileMgr,
               V1BLEClient* ble,
               V1Display* disp) {
    settings = settingsMgr;
    profiles = profileMgr;
    bleClient = ble;
    display = disp;
}

void AutoPushModule::start(int slotIndex) {
    if (!settings || !profiles || !bleClient || !display) {
        return;
    }

    static const char* slotNames[] = {"Default", "Highway", "Passenger Comfort"};
    int clampedIndex = std::max(0, std::min(2, slotIndex));
    state.slotIndex = clampedIndex;
    state.slot = settings->getSlot(clampedIndex);
    state.profileLoaded = false;
    state.profile = V1Profile();
    state.step = Step::WaitReady;
    state.nextStepAtMs = millis() + 500;
    state.profileWriteRetries = 0;
    AUTO_PUSH_LOGF("[AutoPush] V1 connected - applying '%s' profile (slot %d)...\n",
                   slotNames[clampedIndex], clampedIndex);

    // Show the profile indicator when auto-push begins
    display->drawProfileIndicator(clampedIndex);
}

void AutoPushModule::applySlotMuteToZero(V1UserSettings& userSettings, bool slotMuteToZero) {
    if (slotMuteToZero) {
        // MZ enabled: clear bit 4 (inverted logic)
        userSettings.bytes[0] &= ~0x10;
    } else {
        // MZ disabled: set bit 4
        userSettings.bytes[0] |= 0x10;
    }
}

void AutoPushModule::process() {
    if (state.step == Step::Idle) {
        return;
    }

    if (!bleClient || !bleClient->isConnected()) {
        state.step = Step::Idle;
        return;
    }

    unsigned long now = millis();
    if (now < state.nextStepAtMs) {
        return;
    }

    switch (state.step) {
        case Step::WaitReady:
            state.step = Step::Profile;
            state.nextStepAtMs = now;
            return;

        case Step::Profile: {
            const AutoPushSlot& slot = state.slot;
            if (slot.profileName.length() > 0) {
                AUTO_PUSH_LOGF("[AutoPush] Loading profile: %s\n", slot.profileName.c_str());
                V1Profile profile;
                if (profiles && profiles->loadProfile(slot.profileName, profile)) {
                    state.profile = profile;
                    state.profileLoaded = true;

                    bool slotMuteToZero = settings->getSlotMuteToZero(state.slotIndex);
                    AUTO_PUSH_LOGF("[AutoPush] Slot %d MZ setting: %s\n",
                                   state.slotIndex, slotMuteToZero ? "ON" : "OFF");
                    AUTO_PUSH_LOGF("[AutoPush] Profile byte0 before: 0x%02X\n", profile.settings.bytes[0]);

                    V1UserSettings modifiedSettings = profile.settings;
                    applySlotMuteToZero(modifiedSettings, slotMuteToZero);
                    AUTO_PUSH_LOGF("[AutoPush] Modified byte0: 0x%02X (bit4=%d means MZ=%s)\n",
                                   modifiedSettings.bytes[0],
                                   (modifiedSettings.bytes[0] & 0x10) ? 1 : 0,
                                   (modifiedSettings.bytes[0] & 0x10) ? "OFF" : "ON");

                    if (bleClient->writeUserBytes(modifiedSettings.bytes)) {
                        AUTO_PUSH_LOGF("[AutoPush] Profile settings pushed (MZ=%s)\n",
                                       slotMuteToZero ? "ON" : "OFF");
                        bleClient->startUserBytesVerification(modifiedSettings.bytes);
                        state.step = Step::ProfileReadback;
                        state.nextStepAtMs = now + 100;
                        return;
                    } else {
                        if (state.profileWriteRetries < kMaxProfileWriteRetries) {
                            state.profileWriteRetries++;
                            AUTO_PUSH_LOGF("[AutoPush] Write busy, retrying (%u/%u)\n",
                                           state.profileWriteRetries, kMaxProfileWriteRetries);
                            state.step = Step::Profile;
                            state.nextStepAtMs = now + 100;
                            return;
                        }
                        AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to push profile settings");
                    }
                } else {
                    AUTO_PUSH_LOGF("[AutoPush] ERROR: Failed to load profile '%s'\n", slot.profileName.c_str());
                    state.profileLoaded = false;
                }
            } else {
                AUTO_PUSH_LOGLN("[AutoPush] No profile configured for active slot");
                state.profileLoaded = false;
            }

            state.step = Step::Display;
            state.nextStepAtMs = now + 100;
            return;
        }

        case Step::ProfileReadback:
            bleClient->requestUserBytes();
            AUTO_PUSH_LOGLN("[AutoPush] Requested user bytes read-back for verification");
            state.step = Step::Display;
            state.nextStepAtMs = now + 100;
            return;

        case Step::Display: {
            bool slotDarkMode = settings->getSlotDarkMode(state.slotIndex);
            bool displayOn = !slotDarkMode;
            bleClient->setDisplayOn(displayOn);
            AUTO_PUSH_LOGF("[AutoPush] Display set to: %s (darkMode=%s)\n",
                           displayOn ? "ON" : "OFF", slotDarkMode ? "true" : "false");
            state.step = Step::Mode;
            state.nextStepAtMs = now + (state.slot.mode != V1_MODE_UNKNOWN ? 100 : 0);
            return;
        }

        case Step::Mode: {
            if (state.slot.mode != V1_MODE_UNKNOWN) {
                const char* modeName = "Unknown";
                if (state.slot.mode == V1_MODE_ALL_BOGEYS) modeName = "All Bogeys";
                else if (state.slot.mode == V1_MODE_LOGIC) modeName = "Logic";
                else if (state.slot.mode == V1_MODE_ADVANCED_LOGIC) modeName = "Advanced Logic";

                if (bleClient->setMode(static_cast<uint8_t>(state.slot.mode))) {
                    AUTO_PUSH_LOGF("[AutoPush] Mode set to: %s\n", modeName);
                } else {
                    AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to set mode");
                }
            }

            bool volumeChangeNeeded =
                (settings->getSlotVolume(state.slotIndex) != 0xFF ||
                 settings->getSlotMuteVolume(state.slotIndex) != 0xFF);
            state.step = Step::Volume;
            state.nextStepAtMs = now + (volumeChangeNeeded ? 100 : 0);
            return;
        }

        case Step::Volume: {
            uint8_t mainVol = settings->getSlotVolume(state.slotIndex);
            uint8_t muteVol = settings->getSlotMuteVolume(state.slotIndex);
            if (mainVol != 0xFF || muteVol != 0xFF) {
                if (bleClient->setVolume(mainVol, muteVol)) {
                    AUTO_PUSH_LOGF("[AutoPush] Volume set - main: %d, muted: %d\n", mainVol, muteVol);
                } else {
                    AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to set volume");
                }
            }

            AUTO_PUSH_LOGLN("[AutoPush] Complete");
            state.step = Step::Idle;
            state.nextStepAtMs = 0;
            return;
        }

        default:
            state.step = Step::Idle;
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
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"slot\":%d,\"step\":\"%s\",\"profileLoaded\":%s,\"profileConfigured\":%s,\"profileName\":\"%s\"}",
             state.step == Step::Idle ? "false" : "true",
             state.slotIndex,
             stepName,
             state.profileLoaded ? "true" : "false",
             hasProfile ? "true" : "false",
             profileName);
    return String(buf);
}
