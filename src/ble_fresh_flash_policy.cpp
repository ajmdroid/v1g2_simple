#include "ble_fresh_flash_policy.h"

namespace BleFreshFlashPolicy {

namespace {

const char* normalizedVersion(const char* version) {
    return version ? version : "";
}

}  // namespace

String readStoredFirmwareVersion(Preferences& prefs) {
    return prefs.getString(kFirmwareVersionKey, "");
}

bool hasFirmwareVersionMismatch(Preferences& prefs, const char* currentVersion) {
    return readStoredFirmwareVersion(prefs) != normalizedVersion(currentVersion);
}

bool storeFirmwareVersion(Preferences& prefs, const char* currentVersion) {
    prefs.putString(kFirmwareVersionKey, normalizedVersion(currentVersion));
    return readStoredFirmwareVersion(prefs) == normalizedVersion(currentVersion);
}

}  // namespace BleFreshFlashPolicy
