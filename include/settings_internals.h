/**
 * Shared internals for settings translation units.
 *
 * Include this instead of settings.h in the settings_*.cpp files
 * so that cross-TU constants, crypto helpers, and backup utilities
 * are available without duplicating declarations.
 */

#ifndef SETTINGS_INTERNALS_H
#define SETTINGS_INTERNALS_H

#include "settings.h"
#include "settings_namespace_ids.h"
#include "settings_sanitize.h"
#include "storage_manager.h"
#include "v1_profiles.h"
#include <ArduinoJson.h>
#include <algorithm>

// ── Shared NVS / SD constants ──────────────────────────────────────────────

extern const char* SETTINGS_BACKUP_PATH;
extern const char* SETTINGS_BACKUP_TMP_PATH;
extern const char* SETTINGS_BACKUP_PREV_PATH;
inline constexpr int SD_BACKUP_VERSION = 11;
extern const size_t SETTINGS_BACKUP_MAX_BYTES;
extern const char* WIFI_CLIENT_NS;
extern const char* WIFI_CLIENT_SD_SECRET_PATH;
extern const char* WIFI_CLIENT_SD_SECRET_TYPE;
extern const int   WIFI_CLIENT_SD_SECRET_VERSION;
extern const char* const SETTINGS_BACKUP_CANDIDATES[];
extern const size_t      SETTINGS_BACKUP_CANDIDATES_COUNT;

extern const char  XOR_KEY[];
inline constexpr int SETTINGS_VERSION = 8;
extern const char* OBFUSCATION_HEX_PREFIX;

// ── Static helpers promoted to internal-linkage-free functions ──────────────

WiFiModeSetting clampWifiModeValue(int raw);
VoiceAlertMode  clampVoiceAlertModeValue(int raw);
String sanitizeApPasswordValue(const String& raw);
String sanitizeLastV1AddressValue(const String& raw);

// Backup file helpers
bool isSupportedBackupType(const JsonDocument& doc);
bool hasBackupSignature(const JsonDocument& doc);
bool parseBackupFile(fs::FS* fs, const char* path, JsonDocument& doc, bool verboseErrors = true);
int  backupDocumentVersion(const JsonDocument& doc);
int  backupCriticalFieldScore(const JsonDocument& doc);
int  backupCandidateScore(const JsonDocument& doc);
bool loadBestBackupDocument(fs::FS* fs, JsonDocument& outDoc,
                            const char** outPath = nullptr, bool verboseErrors = false);
bool parseBoolVariant(const JsonVariantConst& value, bool& out);
bool writeBackupAtomically(fs::FS* fs, const JsonDocument& doc);

// NVS helpers
bool   attemptNvsRecovery(const char* activeNs);
int    namespaceHealthScore(const char* ns);
bool   isKnownSettingsNamespace(const String& ns);

struct SettingsNamespaceCleanupPlan {
    bool shouldCleanup = false;
    const char* inactiveNamespace = nullptr;
    bool clearLegacyNamespace = false;
};

inline SettingsNamespaceCleanupPlan buildSettingsNamespaceCleanupPlan(uint32_t usedPct,
                                                                     const String& activeNs,
                                                                     bool hasSdBackup) {
    SettingsNamespaceCleanupPlan plan;
    if (usedPct <= 80) {
        return plan;
    }

    if (activeNs == SETTINGS_NS_A) {
        plan.shouldCleanup = true;
        plan.inactiveNamespace = SETTINGS_NS_B;
        plan.clearLegacyNamespace = hasSdBackup;
        return plan;
    }
    if (activeNs == SETTINGS_NS_B) {
        plan.shouldCleanup = true;
        plan.inactiveNamespace = SETTINGS_NS_A;
        plan.clearLegacyNamespace = hasSdBackup;
        return plan;
    }

    // If the active namespace is legacy or unknown, avoid destructive cleanup.
    return plan;
}

// Crypto / obfuscation
String xorObfuscate(const String& input);
char   hexDigit(uint8_t nibble);
int    hexNibble(char c);
String bytesToHex(const String& input);
bool   hexToBytes(const String& input, String& out);
String encodeObfuscatedForStorage(const String& plainText);
String decodeObfuscatedFromStorage(const String& stored);

// WiFi client SD secret helpers
bool   saveWifiClientSecretToSD(const String& ssid, const String& encodedPassword);
String loadWifiClientSecretFromSD(const String& expectedSsid);
void   clearWifiClientSecretFromSD();

#endif // SETTINGS_INTERNALS_H
