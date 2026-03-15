#ifndef BLE_FRESH_FLASH_POLICY_H
#define BLE_FRESH_FLASH_POLICY_H

#include <Arduino.h>
#include <Preferences.h>

namespace BleFreshFlashPolicy {

constexpr const char* kNamespace = "ble_state";
constexpr const char* kFirmwareVersionKey = "fwVersion";

String readStoredFirmwareVersion(Preferences& prefs);
bool hasFirmwareVersionMismatch(Preferences& prefs, const char* currentVersion);
bool storeFirmwareVersion(Preferences& prefs, const char* currentVersion);

}  // namespace BleFreshFlashPolicy

#endif  // BLE_FRESH_FLASH_POLICY_H
