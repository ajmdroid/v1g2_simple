/**
 * OTA API Service — implementation.
 *
 * State machine for checking GitHub Releases, downloading firmware/filesystem
 * binaries, verifying SHA-256, and flashing via the Arduino Update library.
 *
 * Design constraints:
 * - No std::function (ARCHITECTURE.md)
 * - No blocking in loop() — state machine yields between steps
 * - BLE fully deinited before any flash writes
 * - Downloads streamed to flash, not buffered in RAM
 * - GitHub API rate limit: max 1 check per session (cached)
 */

#include "ota_api_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <WiFi.h>

#include "ble_client.h"
#include "wifi_manager.h"
#include "display.h"
#include "config.h"
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>

// getBuildGitSha() is defined in build_metadata.cpp with no header.
extern const char* getBuildGitSha();

// ============================================================================
// Constants
// ============================================================================

static const char* TAG = "OTA";

/// GitHub API endpoint for latest release.
static const char* GITHUB_RELEASES_URL =
    "https://api.github.com/repos/ajmdroid/v1g2_simple/releases/latest";

/// OTA slot capacity from partitions_v1.csv (ota_0 and ota_1 are both 0x6C0000).
static constexpr size_t OTA_SLOT_CAPACITY = 0x6C0000;  // 7,077,888 bytes

/// HTTP timeout for GitHub API and binary downloads.
static constexpr int HTTP_TIMEOUT_MS = 15000;

/// Stream buffer size for downloading firmware chunks.
static constexpr size_t DOWNLOAD_BUFFER_SIZE = 4096;

/// DigiCert Global Root G2 — root CA for github.com and objects.githubusercontent.com.
/// Valid until 2038-01-15. If GitHub changes their CA chain, update this cert.
/// Source: https://cacerts.digicert.com/DigiCertGlobalRootG2.crt.pem
static const char* GITHUB_ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wzt
CO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQv
IOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMCcit6E7UMYLBR/rvHBFVtFN2KoI5yTMfKIXqQmVt8Pv/Q5e9ZvSbW/LTV
Emh+mJmjFCEi/hPhoKNPJoqquvJxHLAeubEq1EkPQFCGfGi6gLGIJg/e5NN1WQYP
jxCOdHbPBAy6ByJyF6vWz0DgB6TQR0Pnj7M0R8bS+JOOdmBJ/r1nS2OeZ1D9RRF
t3O7Q4UfFmFBg0MWS5IRwjifBftmCaFZ9Ul+NN0iC0/1ItYFLUCM9qVPXMzjYdXN
K5WBxJY5RVbTD8KWaXEXUK3q2CJkgERRmm1TAyUPgR+B3xmR5DnKSFRKRTmEJYg
pSo=
-----END CERTIFICATE-----
)EOF";

/// Maximum download retry attempts for transient network failures.
static constexpr int MAX_DOWNLOAD_RETRIES = 3;

/// Delay between retry attempts (ms). Doubles on each retry.
static constexpr uint32_t RETRY_BASE_DELAY_MS = 2000;

/// Minimum interval between /api/ota/check requests (ms).
/// Prevents the UI from hammering the GitHub API.
static constexpr uint32_t OTA_CHECK_MIN_INTERVAL_MS = 300000;  // 5 minutes

/// NVS namespace and keys for two-phase "both" update.
/// After firmware is flashed, we persist the filesystem update details
/// so the new firmware can complete phase 2 on its first boot.
static const char* NVS_OTA_NS         = "ota_pending";
static const char* NVS_KEY_FS_URL     = "fs_url";
static const char* NVS_KEY_FS_SIZE    = "fs_size";
static const char* NVS_KEY_FS_SHA     = "fs_sha256";
static const char* NVS_KEY_FS_VERSION = "fs_ver";

// ============================================================================
// OTA state
// ============================================================================

enum class OtaState : uint8_t {
    IDLE,
    CHECK_REQUESTED,
    CHECKING,
    UPDATE_AVAILABLE,
    NO_UPDATE,
    CHECK_FAILED,
    BLE_SHUTTING_DOWN,
    DOWNLOADING_FIRMWARE,
    DOWNLOADING_FILESYSTEM,
    FS_PENDING_WIFI,        // Phase 2: waiting for WiFi STA before FS download
    RESTARTING,
    CANCELLED,
    ERROR,
};

/// Cached manifest data from the last successful version check.
struct OtaManifest {
    String version;
    String minFromVersion;
    String firmwareFile;
    size_t firmwareSize = 0;
    String firmwareSha256;
    String filesystemFile;
    size_t filesystemSize = 0;
    String filesystemSha256;
    bool breaking = false;
    String changelog;
    String notes;
    String firmwareUrl;     // Resolved GitHub asset download URL
    String filesystemUrl;   // Resolved GitHub asset download URL
};

/// What the user requested to update.
enum class OtaTarget : uint8_t {
    FIRMWARE,
    FILESYSTEM,
    BOTH,
};

// File-scoped state
static V1BLEClient* ble_ = nullptr;
static WiFiManager* wifi_ = nullptr;
static V1Display* display_ = nullptr;
static void (*pumpServer_)(void* ctx) = nullptr;
static void* pumpServerCtx_ = nullptr;

static OtaState state_ = OtaState::IDLE;
static OtaManifest manifest_;
static bool checkDone_ = false;          // True after first check this session
static OtaTarget updateTarget_ = OtaTarget::BOTH;
static int progressPercent_ = 0;
static String errorMessage_;
static uint32_t bleShutdownStartMs_ = 0;
static volatile bool cancelRequested_ = false;
static int downloadRetryCount_ = 0;          // Current retry attempt for transient failures
static uint32_t lastCheckRequestMs_ = 0;     // Timestamp of last /api/ota/check acceptance

// ============================================================================
// Semver comparison
// ============================================================================

/// Parse "MAJOR.MINOR.PATCH" into a comparable integer.
/// Each component is packed into 8 bits, so values must be 0-255.
/// This is safe for this project (semver enforced by CI script).
/// Returns 0 on parse failure.
static uint32_t semverToInt(const String& ver) {
    int dot1 = ver.indexOf('.');
    if (dot1 < 0) return 0;
    int dot2 = ver.indexOf('.', dot1 + 1);
    if (dot2 < 0) return 0;

    uint32_t major = ver.substring(0, dot1).toInt();
    uint32_t minor = ver.substring(dot1 + 1, dot2).toInt();
    uint32_t patch = ver.substring(dot2 + 1).toInt();

    return (major << 16) | (minor << 8) | patch;
}

/// Returns true if `current` is older than `available`.
static bool isNewerVersion(const String& current, const String& available) {
    return semverToInt(available) > semverToInt(current);
}

/// Returns true if `current` is at least `minFrom` (can upgrade directly).
static bool canUpgradeFrom(const String& current, const String& minFrom) {
    return semverToInt(current) >= semverToInt(minFrom);
}

// ============================================================================
// NVS helpers — two-phase filesystem update persistence
// ============================================================================

/// Save pending filesystem update to NVS so the new firmware can
/// complete phase 2 on its first boot (after firmware restart).
static void savePendingFilesystemUpdate(const OtaManifest& m) {
    Preferences prefs;
    if (!prefs.begin(NVS_OTA_NS, false)) {
        Serial.printf("[%s] ERROR: NVS open failed for pending FS write\n", TAG);
        return;
    }
    prefs.putString(NVS_KEY_FS_URL, m.filesystemUrl);
    prefs.putUInt(NVS_KEY_FS_SIZE, m.filesystemSize);
    prefs.putString(NVS_KEY_FS_SHA, m.filesystemSha256);
    prefs.putString(NVS_KEY_FS_VERSION, m.version);
    prefs.end();
    Serial.printf("[%s] Saved pending filesystem update to NVS (%zu bytes)\n",
                  TAG, m.filesystemSize);
}

/// Check NVS for a pending filesystem update. Returns true if one exists,
/// populating the url/size/sha256 output params.
static bool loadPendingFilesystemUpdate(String& url, size_t& size,
                                        String& sha256, String& version) {
    Preferences prefs;
    if (!prefs.begin(NVS_OTA_NS, true)) return false;  // Read-only
    url = prefs.getString(NVS_KEY_FS_URL, "");
    size = prefs.getUInt(NVS_KEY_FS_SIZE, 0);
    sha256 = prefs.getString(NVS_KEY_FS_SHA, "");
    version = prefs.getString(NVS_KEY_FS_VERSION, "");
    prefs.end();
    return !url.isEmpty() && size > 0;
}

/// Clear pending filesystem update from NVS (after successful flash or on error).
static void clearPendingFilesystemUpdate() {
    Preferences prefs;
    if (!prefs.begin(NVS_OTA_NS, false)) return;
    prefs.clear();
    prefs.end();
    Serial.printf("[%s] Cleared pending filesystem update from NVS\n", TAG);
}

// ============================================================================
// TLS helpers
// ============================================================================

/// Shared WiFiClientSecure instance for OTA HTTPS connections.
/// Reused across API and download calls to save RAM (TLS context is ~40KB).
/// Not thread-safe — but all callers run on the same loop task.
static WiFiClientSecure* tlsClient_ = nullptr;

/// Get or create the TLS client with GitHub root CA pinned.
static WiFiClientSecure& getTlsClient() {
    if (!tlsClient_) {
        tlsClient_ = new WiFiClientSecure();
        tlsClient_->setCACert(GITHUB_ROOT_CA);
    }
    return *tlsClient_;
}

/// Free the TLS client (call after OTA work is done to reclaim ~40KB).
static void freeTlsClient() {
    if (tlsClient_) {
        delete tlsClient_;
        tlsClient_ = nullptr;
    }
}

// ============================================================================
// Time sync for TLS — cert validation requires wall-clock time
// ============================================================================

/// Ensure the system clock is set (year > 2024) before TLS connections.
/// Configures SNTP on first call, then waits up to 10 seconds for sync.
/// Returns true if time is now valid.
static bool ensureTimeSync() {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    if (t.tm_year >= (2024 - 1900)) {
        return true;  // Already synced.
    }

    Serial.printf("[%s] System time not set (year=%d) — starting SNTP sync\n",
                  TAG, t.tm_year + 1900);
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    // Wait for sync with timeout.
    uint32_t start = millis();
    while (millis() - start < 10000) {
        now = time(nullptr);
        gmtime_r(&now, &t);
        if (t.tm_year >= (2024 - 1900)) {
            Serial.printf("[%s] SNTP sync OK — %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                          TAG, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                          t.tm_hour, t.tm_min, t.tm_sec);
            return true;
        }
        delay(100);
    }

    Serial.printf("[%s] SNTP sync timed out — TLS may fail\n", TAG);
    return false;
}

// ============================================================================
// GitHub API helpers
// ============================================================================

/// Fetch the latest release JSON from GitHub and extract asset URLs.
/// Populates manifest_ on success. Returns true on success.
static bool fetchLatestRelease() {
    // TLS cert validation requires wall-clock time.
    if (!ensureTimeSync()) {
        errorMessage_ = "Clock sync failed — cannot verify GitHub certificate";
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent("V1Simple-OTA/" FIRMWARE_VERSION);

    http.begin(getTlsClient(), GITHUB_RELEASES_URL);
    http.addHeader("Accept", "application/vnd.github+json");

    Serial.printf("[%s] Checking %s\n", TAG, GITHUB_RELEASES_URL);
    int httpCode = http.GET();

    if (httpCode != 200) {
        Serial.printf("[%s] GitHub API returned %d\n", TAG, httpCode);
        if (httpCode < 0) {
            errorMessage_ = "Connection to GitHub failed (TLS/network error)";
        } else if (httpCode == 403) {
            errorMessage_ = "GitHub API rate limit exceeded — try again later";
        } else {
            errorMessage_ = "GitHub API error: HTTP " + String(httpCode);
        }
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Parse the release JSON to find ota-manifest.json asset URL.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[%s] JSON parse error: %s\n", TAG, err.c_str());
        errorMessage_ = "Failed to parse GitHub response";
        return false;
    }

    // Find the ota-manifest.json, firmware.bin, and littlefs.bin asset URLs.
    String manifestUrl;
    JsonArray assets = doc["assets"];
    for (JsonObject asset : assets) {
        const char* name = asset["name"];
        const char* url = asset["browser_download_url"];
        if (!name || !url) continue;

        if (strcmp(name, "ota-manifest.json") == 0) {
            manifestUrl = url;
        } else if (strcmp(name, "firmware.bin") == 0) {
            manifest_.firmwareUrl = url;
        } else if (strcmp(name, "littlefs.bin") == 0) {
            manifest_.filesystemUrl = url;
        }
    }

    if (manifestUrl.isEmpty()) {
        Serial.printf("[%s] No ota-manifest.json in release assets\n", TAG);
        errorMessage_ = "Release has no OTA manifest (older release?)";
        return false;
    }

    // Fetch the manifest itself.
    HTTPClient manifestHttp;
    manifestHttp.setConnectTimeout(HTTP_TIMEOUT_MS);
    manifestHttp.setTimeout(HTTP_TIMEOUT_MS);
    manifestHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    manifestHttp.begin(getTlsClient(), manifestUrl);

    httpCode = manifestHttp.GET();
    if (httpCode != 200) {
        Serial.printf("[%s] Manifest fetch returned %d\n", TAG, httpCode);
        errorMessage_ = "Manifest fetch error: HTTP " + String(httpCode);
        manifestHttp.end();
        return false;
    }

    String manifestPayload = manifestHttp.getString();
    manifestHttp.end();

    // Parse manifest.
    JsonDocument manifestDoc;
    err = deserializeJson(manifestDoc, manifestPayload);
    if (err) {
        Serial.printf("[%s] Manifest JSON parse error: %s\n", TAG, err.c_str());
        errorMessage_ = "Failed to parse OTA manifest";
        return false;
    }

    manifest_.version = manifestDoc["version"].as<String>();
    manifest_.minFromVersion = manifestDoc["min_from_version"] | "0.0.0";
    manifest_.firmwareFile = manifestDoc["firmware"]["file"] | "firmware.bin";
    manifest_.firmwareSize = manifestDoc["firmware"]["size"] | 0;
    manifest_.firmwareSha256 = manifestDoc["firmware"]["sha256"] | "";
    manifest_.filesystemFile = manifestDoc["filesystem"]["file"] | "littlefs.bin";
    manifest_.filesystemSize = manifestDoc["filesystem"]["size"] | 0;
    manifest_.filesystemSha256 = manifestDoc["filesystem"]["sha256"] | "";
    manifest_.breaking = manifestDoc["breaking"] | false;
    manifest_.changelog = manifestDoc["changelog"] | "";
    manifest_.notes = manifestDoc["notes"] | "";

    // Validate that we found download URLs for the binaries.
    if (manifest_.firmwareUrl.isEmpty()) {
        Serial.printf("[%s] WARNING: firmware.bin not found in release assets\n", TAG);
    }
    if (manifest_.filesystemUrl.isEmpty()) {
        Serial.printf("[%s] WARNING: littlefs.bin not found in release assets\n", TAG);
    }
    if (manifest_.firmwareUrl.isEmpty() && manifest_.filesystemUrl.isEmpty()) {
        errorMessage_ = "No firmware or filesystem binaries in release";
        return false;
    }

    Serial.printf("[%s] Latest release: %s (current: %s)\n",
                  TAG, manifest_.version.c_str(), FIRMWARE_VERSION);

    return true;
}

// ============================================================================
// Download + flash helpers
// ============================================================================

/// Stream-download a binary from `url`, verify SHA-256 against `expectedSha256`,
/// and write to flash via Update library. `command` is U_FLASH or U_SPIFFS.
/// Returns true on success.
static bool downloadAndFlash(const String& url,
                             size_t expectedSize,
                             const String& expectedSha256,
                             int command) {
    if (url.isEmpty()) {
        errorMessage_ = "No download URL for binary";
        return false;
    }
    if (expectedSize == 0) {
        errorMessage_ = "Expected size is 0";
        return false;
    }
    if (expectedSize > OTA_SLOT_CAPACITY && command == U_FLASH) {
        errorMessage_ = "Firmware too large for OTA slot";
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(30000);  // Longer timeout for large binary downloads
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(getTlsClient(), url);

    int httpCode = http.GET();
    if (httpCode != 200) {
        errorMessage_ = "Download error: HTTP " + String(httpCode);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    if (contentLen <= 0) {
        // Chunked transfer — use expected size from manifest.
        contentLen = expectedSize;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        errorMessage_ = "Failed to get download stream";
        http.end();
        return false;
    }

    // Stream download with SHA-256 verification.
    mbedtls_sha256_context sha256Ctx;
    mbedtls_sha256_init(&sha256Ctx);
    mbedtls_sha256_starts(&sha256Ctx, 0);  // 0 = SHA-256 (not SHA-224)
    uint8_t hash_unused[32];  // Discard buffer for error-path finish() calls.

    size_t totalWritten = 0;

    // Validate magic byte for firmware (first byte should be 0xE9).
    if (command == U_FLASH) {
        uint8_t magicByte;
        if (stream->readBytes(&magicByte, 1) != 1) {
            errorMessage_ = "Failed to read magic byte";
            mbedtls_sha256_finish(&sha256Ctx, hash_unused);
            mbedtls_sha256_free(&sha256Ctx);
            http.end();
            return false;
        }
        if (magicByte != 0xE9) {
            errorMessage_ = "Invalid firmware magic byte: 0x" + String(magicByte, HEX);
            mbedtls_sha256_finish(&sha256Ctx, hash_unused);
            mbedtls_sha256_free(&sha256Ctx);
            http.end();
            return false;
        }

        // Start Update and write the magic byte first.
        if (!Update.begin(contentLen, command)) {
            errorMessage_ = "Update.begin failed: " + String(Update.errorString());
            mbedtls_sha256_finish(&sha256Ctx, hash_unused);
            mbedtls_sha256_free(&sha256Ctx);
            http.end();
            return false;
        }
        if (Update.write(&magicByte, 1) != 1) {
            errorMessage_ = "Failed to write magic byte";
            Update.abort();
            mbedtls_sha256_finish(&sha256Ctx, hash_unused);
            mbedtls_sha256_free(&sha256Ctx);
            http.end();
            return false;
        }

        // Hash the actual byte read from the stream (not a hardcoded value).
        mbedtls_sha256_update(&sha256Ctx, &magicByte, 1);
        totalWritten = 1;
    } else {
        // Filesystem — no magic byte check.
        if (!Update.begin(contentLen, command)) {
            errorMessage_ = "Update.begin failed: " + String(Update.errorString());
            mbedtls_sha256_finish(&sha256Ctx, hash_unused);
            mbedtls_sha256_free(&sha256Ctx);
            http.end();
            return false;
        }
    }

    uint8_t buf[DOWNLOAD_BUFFER_SIZE];
    uint32_t lastProgressMs = millis();

    while (totalWritten < (size_t)contentLen) {
        // Yield to system tasks and check for cancel periodically.
        if (millis() - lastProgressMs > 500) {
            progressPercent_ = (int)((totalWritten * 100ULL) / contentLen);
            lastProgressMs = millis();

            // Update LCD progress display.
            if (display_) {
                const char* phase = (command == U_FLASH)
                    ? "Downloading firmware..."
                    : "Downloading filesystem...";
                display_->showOtaProgress(
                    static_cast<uint8_t>(progressPercent_), phase);
            }

            // Pump the WebServer so cancel requests can arrive.
            if (pumpServer_) pumpServer_(pumpServerCtx_);

            // Check cancel flag (set by handleApiOtaCancel via pumped server).
            if (cancelRequested_) {
                Serial.printf("[%s] Download cancelled by user\n", TAG);
                errorMessage_ = "Update cancelled";
                Update.abort();
                mbedtls_sha256_finish(&sha256Ctx, hash_unused);
                mbedtls_sha256_free(&sha256Ctx);
                http.end();
                return false;
            }

            yield();
        }

        size_t available = stream->available();
        if (available == 0) {
            // Wait for data with timeout.
            uint32_t waitStart = millis();
            while (stream->available() == 0 && stream->connected()) {
                if (millis() - waitStart > 10000) {
                    errorMessage_ = "Download timeout (stalled)";
                    Update.abort();
                    mbedtls_sha256_finish(&sha256Ctx, hash_unused);
                    mbedtls_sha256_free(&sha256Ctx);
                    http.end();
                    return false;
                }
                delay(10);
            }
            if (!stream->connected()) {
                errorMessage_ = "Download connection lost";
                Update.abort();
                mbedtls_sha256_finish(&sha256Ctx, hash_unused);
                mbedtls_sha256_free(&sha256Ctx);
                http.end();
                return false;
            }
            continue;
        }

        size_t toRead = min(available, sizeof(buf));
        toRead = min(toRead, (size_t)contentLen - totalWritten);
        size_t bytesRead = stream->readBytes(buf, toRead);
        if (bytesRead == 0) continue;

        mbedtls_sha256_update(&sha256Ctx, buf, bytesRead);

        size_t written = Update.write(buf, bytesRead);
        if (written != bytesRead) {
            errorMessage_ = "Flash write error: " + String(Update.errorString());
            Update.abort();
            mbedtls_sha256_finish(&sha256Ctx, hash_unused);
            mbedtls_sha256_free(&sha256Ctx);
            http.end();
            return false;
        }

        totalWritten += written;
    }

    http.end();
    progressPercent_ = 100;

    // Finalize SHA-256 hash (always call finish before free).
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha256Ctx, hash);
    mbedtls_sha256_free(&sha256Ctx);

    // Verify SHA-256 if manifest included a hash.
    if (!expectedSha256.isEmpty()) {
        char hashHex[65];
        for (int i = 0; i < 32; i++) {
            sprintf(hashHex + (i * 2), "%02x", hash[i]);
        }
        hashHex[64] = '\0';

        if (expectedSha256 != hashHex) {
            Serial.printf("[%s] SHA-256 mismatch!\n  Expected: %s\n  Got:      %s\n",
                          TAG, expectedSha256.c_str(), hashHex);
            errorMessage_ = "SHA-256 verification failed";
            Update.abort();
            return false;
        }

        Serial.printf("[%s] SHA-256 verified OK\n", TAG);
    } else {
        Serial.printf("[%s] WARNING: No SHA-256 in manifest, skipping verification\n", TAG);
    }

    // Finalize the update.
    if (!Update.end(true)) {
        errorMessage_ = "Update.end failed: " + String(Update.errorString());
        return false;
    }

    Serial.printf("[%s] Flash write complete (%zu bytes)\n", TAG, totalWritten);
    return true;
}

// ============================================================================
// Public API
// ============================================================================

namespace OtaApiService {

void begin(V1BLEClient* ble, WiFiManager* wifi, V1Display* display,
           void (*pumpServer)(void* ctx), void* pumpServerCtx) {
    ble_ = ble;
    wifi_ = wifi;
    display_ = display;
    pumpServer_ = pumpServer;
    pumpServerCtx_ = pumpServerCtx;
    state_ = OtaState::IDLE;
    checkDone_ = false;
    cancelRequested_ = false;
    errorMessage_ = "";

    // Two-phase update: check if a previous firmware OTA left a pending
    // filesystem update in NVS. If so, we need to complete phase 2 once
    // WiFi STA connects.
    String fsUrl, fsSha, fsVer;
    size_t fsSize = 0;
    if (loadPendingFilesystemUpdate(fsUrl, fsSize, fsSha, fsVer)) {
        Serial.printf("[%s] Pending filesystem update found (v%s, %zu bytes)\n",
                      TAG, fsVer.c_str(), fsSize);
        // Populate manifest fields needed for the filesystem download.
        manifest_.filesystemUrl = fsUrl;
        manifest_.filesystemSize = fsSize;
        manifest_.filesystemSha256 = fsSha;
        manifest_.version = fsVer;
        updateTarget_ = OtaTarget::FILESYSTEM;
        state_ = OtaState::FS_PENDING_WIFI;
    }

    Serial.printf("[%s] Service initialized (state=%s)\n", TAG,
                  state_ == OtaState::FS_PENDING_WIFI ? "fs_pending_wifi" : "idle");
}

void handleApiVersion(WebServer& server) {
    JsonDocument doc;
    doc["firmware_version"] = FIRMWARE_VERSION;
    doc["git_sha"] = getBuildGitSha();

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiOtaStatus(WebServer& server) {
    JsonDocument doc;

    const char* stateStr = "idle";
    switch (state_) {
        case OtaState::IDLE:                  stateStr = "idle"; break;
        case OtaState::CHECK_REQUESTED:       stateStr = "checking"; break;
        case OtaState::CHECKING:              stateStr = "checking"; break;
        case OtaState::UPDATE_AVAILABLE:      stateStr = "update_available"; break;
        case OtaState::NO_UPDATE:             stateStr = "up_to_date"; break;
        case OtaState::CHECK_FAILED:          stateStr = "check_failed"; break;
        case OtaState::BLE_SHUTTING_DOWN:     stateStr = "preparing"; break;
        case OtaState::DOWNLOADING_FIRMWARE:  stateStr = "downloading_firmware"; break;
        case OtaState::DOWNLOADING_FILESYSTEM: stateStr = "downloading_filesystem"; break;
        case OtaState::FS_PENDING_WIFI:       stateStr = "fs_pending_wifi"; break;
        case OtaState::RESTARTING:            stateStr = "restarting"; break;
        case OtaState::CANCELLED:             stateStr = "cancelled"; break;
        case OtaState::ERROR:                 stateStr = "error"; break;
    }

    doc["state"] = stateStr;
    doc["current_version"] = FIRMWARE_VERSION;
    doc["check_done"] = checkDone_;

    bool staConnected = wifi_ ? wifi_->isConnected() : false;
    doc["sta_connected"] = staConnected;

    // Include available version info if we have it.
    if (checkDone_ && !manifest_.version.isEmpty()) {
        doc["available_version"] = manifest_.version;
        doc["changelog"] = manifest_.changelog;
        doc["breaking"] = manifest_.breaking;
        doc["notes"] = manifest_.notes;

        // Determine if update is possible.
        bool isNewer = isNewerVersion(FIRMWARE_VERSION, manifest_.version);
        bool versionGateOk = canUpgradeFrom(FIRMWARE_VERSION, manifest_.minFromVersion);
        bool fitsSlot = manifest_.firmwareSize <= OTA_SLOT_CAPACITY;
        bool canUpdate = isNewer && versionGateOk && fitsSlot && staConnected;

        doc["can_update"] = canUpdate;

        if (!isNewer) {
            doc["blocked_reason"] = "already_current";
        } else if (!versionGateOk) {
            doc["blocked_reason"] = "version_too_old";
            doc["min_from_version"] = manifest_.minFromVersion;
        } else if (!fitsSlot) {
            doc["blocked_reason"] = "insufficient_space";
        } else if (!staConnected) {
            doc["blocked_reason"] = "no_sta";
        } else {
            doc["blocked_reason"] = nullptr;
        }
    } else {
        doc["available_version"] = nullptr;
        doc["can_update"] = false;
        doc["blocked_reason"] = staConnected ? nullptr : "no_sta";
    }

    doc["progress"] = progressPercent_;
    doc["error"] = errorMessage_.isEmpty() ? nullptr : errorMessage_.c_str();

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiOtaCheck(WebServer& server,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    // Must be on STA to reach GitHub.
    if (!wifi_ || !wifi_->isConnected()) {
        server.send(400, "application/json",
                    R"({"error":"no_sta","message":"WiFi client not connected"})");
        return;
    }

    // Don't start a check if we're mid-update.
    if (state_ == OtaState::DOWNLOADING_FIRMWARE ||
        state_ == OtaState::DOWNLOADING_FILESYSTEM ||
        state_ == OtaState::BLE_SHUTTING_DOWN ||
        state_ == OtaState::FS_PENDING_WIFI ||
        state_ == OtaState::RESTARTING) {
        server.send(409, "application/json",
                    R"({"error":"update_in_progress"})");
        return;
    }

    // OTA-specific rate limit: at most one GitHub check per interval.
    // This supplements the global rate limiter and prevents the UI from
    // accidentally hammering the unauthenticated GitHub API (60 req/hr).
    uint32_t now = millis();
    if (lastCheckRequestMs_ != 0 &&
        (now - lastCheckRequestMs_) < OTA_CHECK_MIN_INTERVAL_MS) {
        // Already checked recently — return cached result instead of re-checking.
        if (checkDone_) {
            server.send(200, "application/json", R"({"checking":false,"cached":true})");
        } else {
            server.send(429, "application/json",
                        R"({"error":"ota_check_rate_limited","message":"Check again in a few minutes"})");
        }
        return;
    }

    lastCheckRequestMs_ = now;
    state_ = OtaState::CHECK_REQUESTED;
    errorMessage_ = "";
    server.send(200, "application/json", R"({"checking":true})");
}

void handleApiOtaStart(WebServer& server,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;

    // Precondition: must have a valid update available.
    if (state_ != OtaState::UPDATE_AVAILABLE) {
        server.send(400, "application/json",
                    R"({"error":"no_update_available","message":"Run /api/ota/check first"})");
        return;
    }

    if (!wifi_ || !wifi_->isConnected()) {
        server.send(400, "application/json",
                    R"({"error":"no_sta","message":"WiFi client not connected"})");
        return;
    }

    // Parse target from request body (default: "both").
    String targetStr = "both";
    if (server.hasArg("plain")) {
        JsonDocument reqDoc;
        DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
        if (!err && reqDoc["target"].is<const char*>()) {
            targetStr = reqDoc["target"].as<String>();
        }
    }

    if (targetStr == "firmware") {
        updateTarget_ = OtaTarget::FIRMWARE;
    } else if (targetStr == "filesystem") {
        updateTarget_ = OtaTarget::FILESYSTEM;
    } else {
        updateTarget_ = OtaTarget::BOTH;
    }

    progressPercent_ = 0;
    errorMessage_ = "";
    cancelRequested_ = false;
    state_ = OtaState::BLE_SHUTTING_DOWN;
    bleShutdownStartMs_ = millis();

    Serial.printf("[%s] Update started — shutting down BLE\n", TAG);
    server.send(200, "application/json", R"({"started":true})");
}

void handleApiOtaCancel(WebServer& server) {
    // Cancel is only meaningful during download phases.
    if (state_ == OtaState::DOWNLOADING_FIRMWARE ||
        state_ == OtaState::DOWNLOADING_FILESYSTEM) {
        cancelRequested_ = true;
        Serial.printf("[%s] Cancel requested by user\n", TAG);
        server.send(200, "application/json", R"({"cancelled":true})");
        return;
    }

    // BLE_SHUTTING_DOWN happens fast (< 1 second), can't meaningfully cancel.
    // RESTARTING is past the point of no return.
    server.send(400, "application/json",
                R"({"error":"not_cancellable","message":"Update is not in a cancellable state"})");
}

void process(uint32_t nowMs) {
    // Fast exit for steady states (the common case — no work to do).
    if (state_ == OtaState::IDLE || state_ == OtaState::UPDATE_AVAILABLE ||
        state_ == OtaState::NO_UPDATE || state_ == OtaState::CHECK_FAILED ||
        state_ == OtaState::ERROR) {
        return;
    }

    switch (state_) {

    case OtaState::CHECK_REQUESTED: {
        state_ = OtaState::CHECKING;
        Serial.printf("[%s] Starting version check\n", TAG);

        bool ok = fetchLatestRelease();
        checkDone_ = true;
        freeTlsClient();  // Reclaim ~40KB until download phase needs it.

        if (!ok) {
            state_ = OtaState::CHECK_FAILED;
            Serial.printf("[%s] Check failed: %s\n", TAG, errorMessage_.c_str());
            break;
        }

        if (isNewerVersion(FIRMWARE_VERSION, manifest_.version)) {
            state_ = OtaState::UPDATE_AVAILABLE;
            Serial.printf("[%s] Update available: %s -> %s\n",
                          TAG, FIRMWARE_VERSION, manifest_.version.c_str());
        } else {
            state_ = OtaState::NO_UPDATE;
            Serial.printf("[%s] Already up to date (%s)\n", TAG, FIRMWARE_VERSION);
        }
        break;
    }

    case OtaState::BLE_SHUTTING_DOWN: {
        // Show the OTA screen immediately when update begins.
        if (display_) display_->showOtaProgress(0, "Preparing...");
        downloadRetryCount_ = 0;

        // Gracefully shut down BLE before flash writes.
        if (ble_) {
            Serial.printf("[%s] Disconnecting BLE\n", TAG);
            ble_->disconnect();
            ble_->cleanupConnection();
        }

        // Deinit NimBLE stack entirely — frees ~40 KB DRAM, stops RF.
        // true = preserve bonding data in NVS.
        NimBLEDevice::deinit(true);

        // Brief settling time for NimBLE tasks to wind down.
        delay(500);

        Serial.printf("[%s] BLE shutdown complete, heap free: %u\n",
                      TAG, ESP.getFreeHeap());

        // Proceed to download.
        if (updateTarget_ == OtaTarget::FILESYSTEM) {
            state_ = OtaState::DOWNLOADING_FILESYSTEM;
        } else {
            state_ = OtaState::DOWNLOADING_FIRMWARE;
        }
        break;
    }

    case OtaState::DOWNLOADING_FIRMWARE: {
        Serial.printf("[%s] Downloading firmware (%zu bytes), attempt %d/%d\n",
                      TAG, manifest_.firmwareSize,
                      downloadRetryCount_ + 1, MAX_DOWNLOAD_RETRIES);

        bool ok = downloadAndFlash(
            manifest_.firmwareUrl,
            manifest_.firmwareSize,
            manifest_.firmwareSha256,
            U_FLASH);

        if (!ok) {
            if (cancelRequested_) {
                state_ = OtaState::CANCELLED;
                Serial.printf("[%s] Firmware download cancelled\n", TAG);
                break;
            }
            // Retry transient failures (network timeouts, stalls).
            downloadRetryCount_++;
            if (downloadRetryCount_ < MAX_DOWNLOAD_RETRIES) {
                uint32_t retryDelay = RETRY_BASE_DELAY_MS * (1u << (downloadRetryCount_ - 1));
                Serial.printf("[%s] Firmware download failed: %s — retrying in %u ms (%d/%d)\n",
                              TAG, errorMessage_.c_str(), retryDelay,
                              downloadRetryCount_ + 1, MAX_DOWNLOAD_RETRIES);
                if (display_) {
                    char retryMsg[48];
                    snprintf(retryMsg, sizeof(retryMsg),
                             "Retry %d/%d...", downloadRetryCount_ + 1, MAX_DOWNLOAD_RETRIES);
                    display_->showOtaProgress(0, retryMsg);
                }
                delay(retryDelay);
                progressPercent_ = 0;
                // Stay in DOWNLOADING_FIRMWARE — process() will re-enter this case.
                break;
            }
            state_ = OtaState::ERROR;
            Serial.printf("[%s] Firmware download failed after %d attempts: %s\n",
                          TAG, MAX_DOWNLOAD_RETRIES, errorMessage_.c_str());
            break;
        }

        downloadRetryCount_ = 0;  // Reset for filesystem phase.

        // Two-phase update: firmware is flashed — now restart and let
        // the new firmware handle filesystem in phase 2. This ensures
        // the bootloader validates new firmware before we touch the FS.
        if (updateTarget_ == OtaTarget::BOTH) {
            savePendingFilesystemUpdate(manifest_);
            state_ = OtaState::RESTARTING;
        } else {
            state_ = OtaState::RESTARTING;
        }
        break;
    }

    case OtaState::DOWNLOADING_FILESYSTEM: {
        Serial.printf("[%s] Downloading filesystem (%zu bytes), attempt %d/%d\n",
                      TAG, manifest_.filesystemSize,
                      downloadRetryCount_ + 1, MAX_DOWNLOAD_RETRIES);

        bool ok = downloadAndFlash(
            manifest_.filesystemUrl,
            manifest_.filesystemSize,
            manifest_.filesystemSha256,
            U_SPIFFS);

        if (!ok) {
            if (cancelRequested_) {
                state_ = OtaState::CANCELLED;
                Serial.printf("[%s] Filesystem download cancelled\n", TAG);
                break;
            }
            downloadRetryCount_++;
            if (downloadRetryCount_ < MAX_DOWNLOAD_RETRIES) {
                uint32_t retryDelay = RETRY_BASE_DELAY_MS * (1u << (downloadRetryCount_ - 1));
                Serial.printf("[%s] Filesystem download failed: %s — retrying in %u ms (%d/%d)\n",
                              TAG, errorMessage_.c_str(), retryDelay,
                              downloadRetryCount_ + 1, MAX_DOWNLOAD_RETRIES);
                if (display_) {
                    char retryMsg[48];
                    snprintf(retryMsg, sizeof(retryMsg),
                             "Retry %d/%d...", downloadRetryCount_ + 1, MAX_DOWNLOAD_RETRIES);
                    display_->showOtaProgress(0, retryMsg);
                }
                delay(retryDelay);
                progressPercent_ = 0;
                break;
            }
            state_ = OtaState::ERROR;
            Serial.printf("[%s] Filesystem download failed after %d attempts: %s\n",
                          TAG, MAX_DOWNLOAD_RETRIES, errorMessage_.c_str());
            clearPendingFilesystemUpdate();  // Don't retry on next boot.
            break;
        }

        clearPendingFilesystemUpdate();  // Phase 2 complete.
        state_ = OtaState::RESTARTING;
        break;
    }

    case OtaState::FS_PENDING_WIFI: {
        // Phase 2 of a two-phase "both" update. Firmware was flashed on the
        // previous boot; now we need WiFi STA to download the filesystem.
        if (!wifi_ || !wifi_->isConnected()) {
            break;  // Not ready yet — will check again next process() tick.
        }

        Serial.printf("[%s] WiFi STA connected — starting phase 2 filesystem update\n", TAG);
        if (display_) display_->showOtaProgress(0, "Updating filesystem...");

        // BLE is already running on the new firmware. Shut it down for the
        // filesystem flash, same as a normal update.
        state_ = OtaState::BLE_SHUTTING_DOWN;
        break;
    }

    case OtaState::RESTARTING: {
        Serial.printf("[%s] OTA complete — restarting in 2 seconds\n", TAG);
        freeTlsClient();
        if (display_) display_->showOtaProgress(100, "Restarting...");
        delay(2000);  // Give the web UI time to poll status one more time
        ESP.restart();
        break;  // Unreachable, but clean.
    }

    case OtaState::CANCELLED: {
        // BLE was deinited before the download started. The cleanest recovery
        // is a restart — re-initializing NimBLE mid-runtime is fragile.
        Serial.printf("[%s] Update cancelled — restarting to recover BLE\n", TAG);
        freeTlsClient();
        if (display_) display_->showOtaProgress(0, "Cancelled — restarting...");
        delay(2000);
        ESP.restart();
        break;
    }

    // CHECKING is a transient state within CHECK_REQUESTED handling.
    // Steady states are filtered by the early return above.
    default:
        break;
    }
}

}  // namespace OtaApiService
