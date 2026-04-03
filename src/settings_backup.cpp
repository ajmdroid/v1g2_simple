/**
 * Settings SD backup writer and backup-file utilities.
 */

#include "settings_internals.h"
#include "backup_payload_builder.h"

#include <esp_heap_caps.h>

// --- Backup file static helpers ---

namespace {

constexpr size_t SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM = 256u;

size_t roundUpSettingsBackupPayloadCapacity(size_t required) {
    return ((required + SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM - 1u) /
            SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM) *
           SETTINGS_BACKUP_PAYLOAD_GROWTH_QUANTUM;
}

bool writeSerializedBackupAtomically(fs::FS* fs, const char* data, size_t length) {
    if (!fs || !data || length == 0) {
        return false;
    }

    if (fs->exists(SETTINGS_BACKUP_TMP_PATH)) {
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
    }

    File tmp = fs->open(SETTINGS_BACKUP_TMP_PATH, FILE_WRITE);
    if (!tmp) {
        Serial.println("[Settings] Failed to create temp SD backup file");
        return false;
    }

    const size_t written = tmp.write(reinterpret_cast<const uint8_t*>(data), length);
    tmp.flush();
    tmp.close();

    if (written != length) {
        Serial.println("[Settings] Failed to write temp SD backup");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
        return false;
    }

    JsonDocument verifyTmp;
    if (!parseBackupFile(fs, SETTINGS_BACKUP_TMP_PATH, verifyTmp, true)) {
        Serial.println("[Settings] Temp SD backup failed validation");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);
        return false;
    }

    if (fs->exists(SETTINGS_BACKUP_PREV_PATH)) {
        fs->remove(SETTINGS_BACKUP_PREV_PATH);
    }

    bool rotatedPrimary = false;
    if (fs->exists(SETTINGS_BACKUP_PATH)) {
        if (fs->rename(SETTINGS_BACKUP_PATH, SETTINGS_BACKUP_PREV_PATH)) {
            rotatedPrimary = true;
        } else {
            Serial.println("[Settings] ERROR: Failed to rotate primary backup; keeping existing file");
            fs->remove(SETTINGS_BACKUP_TMP_PATH);
            return false;
        }
    }

    if (!fs->rename(SETTINGS_BACKUP_TMP_PATH, SETTINGS_BACKUP_PATH)) {
        Serial.println("[Settings] ERROR: Failed to promote temp backup to primary");
        fs->remove(SETTINGS_BACKUP_TMP_PATH);

        if (rotatedPrimary && fs->exists(SETTINGS_BACKUP_PREV_PATH) && !fs->exists(SETTINGS_BACKUP_PATH)) {
            if (!fs->rename(SETTINGS_BACKUP_PREV_PATH, SETTINGS_BACKUP_PATH)) {
                Serial.println("[Settings] CRITICAL: Failed to rollback previous backup");
            }
        }
        return false;
    }

    JsonDocument verifyPrimary;
    if (!parseBackupFile(fs, SETTINGS_BACKUP_PATH, verifyPrimary, true)) {
        Serial.println("[Settings] ERROR: Promoted backup failed validation");
        fs->remove(SETTINGS_BACKUP_PATH);
        if (fs->exists(SETTINGS_BACKUP_PREV_PATH) && !fs->exists(SETTINGS_BACKUP_PATH)) {
            if (!fs->rename(SETTINGS_BACKUP_PREV_PATH, SETTINGS_BACKUP_PATH)) {
                Serial.println("[Settings] CRITICAL: Failed to restore previous backup after validation failure");
            }
        }
        return false;
    }

    return true;
}

}  // namespace

bool isSupportedBackupType(const JsonDocument& doc) {
    if (!doc["_type"].is<const char*>()) {
        return true;  // Legacy backups may not include a type marker.
    }
    return BackupPayloadBuilder::isRecognizedBackupType(doc["_type"].as<const char*>());
}

bool hasBackupSignature(const JsonDocument& doc) {
    // Require a small signature set to avoid accepting arbitrary JSON blobs.
    return doc["apSSID"].is<const char*>() ||
           doc["brightness"].is<int>() ||
           doc["colorBogey"].is<int>() ||
           doc["slot0Name"].is<const char*>();
}

bool parseBackupFile(fs::FS* fs,
                            const char* path,
                            JsonDocument& doc,
                            bool verboseErrors) {
    if (!fs || !path || path[0] == '\0') {
        return false;
    }

    File file = fs->open(path, FILE_READ);
    if (!file) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to open backup file: %s\n", path);
        }
        return false;
    }

    const size_t size = file.size();
    if (size == 0 || size > SETTINGS_BACKUP_MAX_BYTES) {
        if (verboseErrors) {
            Serial.printf("[Settings] Backup file size invalid (%u bytes): %s\n",
                          static_cast<unsigned int>(size),
                          path);
        }
        file.close();
        return false;
    }

    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        if (verboseErrors) {
            Serial.printf("[Settings] Failed to parse backup '%s': %s\n", path, err.c_str());
        }
        return false;
    }

    if (!isSupportedBackupType(doc)) {
        if (verboseErrors) {
            Serial.printf("[Settings] Unsupported backup type in %s\n", path);
        }
        return false;
    }
    if (!hasBackupSignature(doc)) {
        if (verboseErrors) {
            Serial.printf("[Settings] Backup signature check failed for %s\n", path);
        }
        return false;
    }

    return true;
}

int backupDocumentVersion(const JsonDocument& doc) {
    return doc["_version"] | doc["version"] | 1;
}

int backupCriticalFieldScore(const JsonDocument& doc) {
    int score = 0;
    if (!doc["brightness"].isNull()) score++;
    if (!doc["displayStyle"].isNull()) score++;
    if (!doc["proxyBLE"].isNull()) score++;
    if (!doc["proxyName"].isNull()) score++;
    if (!doc["wifiClientEnabled"].isNull()) score++;
    if (!doc["colorBogey"].isNull()) score++;
    if (!doc["slot0ProfileName"].isNull()) score++;
    if (!doc["slot1ProfileName"].isNull()) score++;
    if (!doc["slot2ProfileName"].isNull()) score++;
    return score;
}

int backupCandidateScore(const JsonDocument& doc) {
    // Prefer newer schema, then richer field coverage.
    return backupDocumentVersion(doc) * 100 + backupCriticalFieldScore(doc);
}


bool writeBackupAtomically(fs::FS* fs, const JsonDocument& doc) {
    if (!fs) {
        return false;
    }

    const size_t required = measureJson(doc) + 1u;
    char* jsonData = static_cast<char*>(heap_caps_malloc(required, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    if (!jsonData) {
        Serial.println("[Settings] Failed to allocate temp serialization buffer");
        return false;
    }

    const size_t length = serializeJson(doc, jsonData, required);
    if (length == 0 || length >= required) {
        free(jsonData);
        Serial.println("[Settings] Failed to serialize SD backup");
        return false;
    }
    jsonData[length] = '\0';

    const bool ok = writeSerializedBackupAtomically(fs, jsonData, length);
    free(jsonData);
    return ok;
}

bool buildSerializedSdBackupPayload(SerializedSettingsBackupPayload& payload,
                                    const V1Settings& settings_,
                                    const V1ProfileManager& profileManager,
                                    uint32_t snapshotMs) {
    releaseSerializedSettingsBackupPayload(payload);

    JsonDocument doc;
    const BackupPayloadBuilder::BuildResult buildResult =
        BackupPayloadBuilder::buildBackupDocument(
            doc,
            settings_,
            profileManager,
            BackupPayloadBuilder::BackupTransport::SdBackup,
            snapshotMs);

    const size_t required = measureJson(doc) + 1u;
    const size_t capacity = roundUpSettingsBackupPayloadCapacity(required);
    char* data = static_cast<char*>(
        heap_caps_malloc(capacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    bool inPsram = true;

    if (data == nullptr) {
        data = static_cast<char*>(
            heap_caps_malloc(capacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        inPsram = false;
    }

    if (data == nullptr) {
        Serial.printf("[Settings] Failed to allocate serialized backup buffer (%lu bytes)\n",
                      static_cast<unsigned long>(capacity));
        return false;
    }

    const size_t length = serializeJson(doc, data, capacity);
    if (length == 0 || length >= capacity) {
        heap_caps_free(data);
        Serial.println("[Settings] Failed to serialize SD backup payload");
        return false;
    }
    data[length] = '\0';

    payload.data = data;
    payload.capacity = capacity;
    payload.length = length;
    payload.inPsram = inPsram;
    payload.snapshotMs = snapshotMs;
    payload.profilesBackedUp = buildResult.profilesBackedUp;
    return true;
}

void releaseSerializedSettingsBackupPayload(SerializedSettingsBackupPayload& payload) {
    if (payload.data != nullptr) {
        heap_caps_free(payload.data);
    }
    payload.data = nullptr;
    payload.capacity = 0;
    payload.length = 0;
    payload.inPsram = false;
    payload.snapshotMs = 0;
    payload.profilesBackedUp = 0;
}

bool writeBackupAtomically(fs::FS* fs, const SerializedSettingsBackupPayload& payload) {
    return writeSerializedBackupAtomically(fs, payload.data, payload.length);
}

// --- Member methods: SD backup write path ---


// Backup display/color settings to SD card

bool SettingsManager::backupToSD() {
    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        return false;  // SD not available, skip silently
    }

    // Acquire SD mutex to protect file I/O.
    // checkDmaHeap=false: backupToSD() is called from save() which runs inside
    // WiFi handlers — WiFi's SRAM buffers reduce DMA heap below the guard
    // thresholds, causing every web-UI save to silently skip the SD backup.
    // The write is small (one JSON file) and infrequent, so bypassing is safe.
    StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex(), /*checkDmaHeap=*/false);
    if (!sdLock) {
        Serial.println("[Settings] Failed to acquire SD mutex for backup");
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) return false;

    SerializedSettingsBackupPayload payload;
    if (!buildSerializedSdBackupPayload(payload, settings_, v1ProfileManager, millis())) {
        return false;
    }

    const bool ok = writeBackupAtomically(fs, payload);
    const int profilesBackedUp = payload.profilesBackedUp;
    releaseSerializedSettingsBackupPayload(payload);

    if (!ok) {
        Serial.println("[Settings] ERROR: Failed to commit SD backup atomically");
        return false;
    }

    Serial.printf("[Settings] Full backup saved to SD card (%d profiles)\n",
                  profilesBackedUp);
    Serial.printf("[Settings] Backed up: slot0Mode=%d, slot1Mode=%d, slot2Mode=%d\n",
                  settings_.slot0_default.mode, settings_.slot1_highway.mode, settings_.slot2_comfort.mode);
    return true;
}
