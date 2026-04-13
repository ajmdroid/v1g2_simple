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
#include "config.h"

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
    RESTARTING,
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

static OtaState state_ = OtaState::IDLE;
static OtaManifest manifest_;
static bool checkDone_ = false;          // True after first check this session
static OtaTarget updateTarget_ = OtaTarget::BOTH;
static int progressPercent_ = 0;
static String errorMessage_;
static uint32_t bleShutdownStartMs_ = 0;

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
// GitHub API helpers
// ============================================================================

/// Fetch the latest release JSON from GitHub and extract asset URLs.
/// Populates manifest_ on success. Returns true on success.
static bool fetchLatestRelease() {
    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setUserAgent("V1Simple-OTA/" FIRMWARE_VERSION);

    // GitHub API — skip TLS cert validation, rely on SHA-256 of binaries.
    // ESP32-S3 WiFiClientSecure with setInsecure() disables cert checks.
    http.begin(GITHUB_RELEASES_URL);
    http.addHeader("Accept", "application/vnd.github+json");

    Serial.printf("[%s] Checking %s\n", TAG, GITHUB_RELEASES_URL);
    int httpCode = http.GET();

    if (httpCode != 200) {
        Serial.printf("[%s] GitHub API returned %d\n", TAG, httpCode);
        errorMessage_ = "GitHub API error: HTTP " + String(httpCode);
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
    manifestHttp.begin(manifestUrl);

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
    http.begin(url);

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
        // Yield to system tasks periodically.
        if (millis() - lastProgressMs > 500) {
            progressPercent_ = (int)((totalWritten * 100ULL) / contentLen);
            lastProgressMs = millis();
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

void begin(V1BLEClient* ble, WiFiManager* wifi) {
    ble_ = ble;
    wifi_ = wifi;
    state_ = OtaState::IDLE;
    checkDone_ = false;
    errorMessage_ = "";
    Serial.printf("[%s] Service initialized\n", TAG);
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
        case OtaState::RESTARTING:            stateStr = "restarting"; break;
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
    if (checkRateLimit && checkRateLimit(rateLimitCtx)) {
        server.send(429, "application/json", R"({"error":"rate_limited"})");
        return;
    }

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
        state_ == OtaState::RESTARTING) {
        server.send(409, "application/json",
                    R"({"error":"update_in_progress"})");
        return;
    }

    state_ = OtaState::CHECK_REQUESTED;
    errorMessage_ = "";
    server.send(200, "application/json", R"({"checking":true})");
}

void handleApiOtaStart(WebServer& server,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && checkRateLimit(rateLimitCtx)) {
        server.send(429, "application/json", R"({"error":"rate_limited"})");
        return;
    }

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
    state_ = OtaState::BLE_SHUTTING_DOWN;
    bleShutdownStartMs_ = millis();

    Serial.printf("[%s] Update started — shutting down BLE\n", TAG);
    server.send(200, "application/json", R"({"started":true})");
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
        Serial.printf("[%s] Downloading firmware (%zu bytes)\n",
                      TAG, manifest_.firmwareSize);

        bool ok = downloadAndFlash(
            manifest_.firmwareUrl,
            manifest_.firmwareSize,
            manifest_.firmwareSha256,
            U_FLASH);

        if (!ok) {
            state_ = OtaState::ERROR;
            Serial.printf("[%s] Firmware download/flash failed: %s\n",
                          TAG, errorMessage_.c_str());
            break;
        }

        // If target is BOTH, continue to filesystem.
        if (updateTarget_ == OtaTarget::BOTH) {
            progressPercent_ = 0;
            state_ = OtaState::DOWNLOADING_FILESYSTEM;
        } else {
            state_ = OtaState::RESTARTING;
        }
        break;
    }

    case OtaState::DOWNLOADING_FILESYSTEM: {
        Serial.printf("[%s] Downloading filesystem (%zu bytes)\n",
                      TAG, manifest_.filesystemSize);

        bool ok = downloadAndFlash(
            manifest_.filesystemUrl,
            manifest_.filesystemSize,
            manifest_.filesystemSha256,
            U_SPIFFS);

        if (!ok) {
            state_ = OtaState::ERROR;
            Serial.printf("[%s] Filesystem download/flash failed: %s\n",
                          TAG, errorMessage_.c_str());
            break;
        }

        state_ = OtaState::RESTARTING;
        break;
    }

    case OtaState::RESTARTING: {
        Serial.printf("[%s] OTA complete — restarting in 2 seconds\n", TAG);
        delay(2000);  // Give the web UI time to poll status one more time
        ESP.restart();
        break;  // Unreachable, but clean.
    }

    // CHECKING is a transient state within CHECK_REQUESTED handling.
    // Steady states are filtered by the early return above.
    default:
        break;
    }
}

}  // namespace OtaApiService
