/**
 * Storage Manager implementation
 * 
 * Mounts SD card (SDMMC) or falls back to LittleFS.
 */

#include "storage_manager.h"
#include <SD_MMC.h>
#include <LittleFS.h>

// Global instance
StorageManager storageManager;

StorageManager::StorageManager()
    : fs(nullptr), ready(false), usingSDMMC(false), littlefsReady(false), sdMutex(nullptr) {
    // Create SD access mutex - critical for thread safety across cores
    sdMutex = xSemaphoreCreateMutex();
    if (!sdMutex) {
        Serial.println("[Storage] CRITICAL: Failed to create SD mutex!");
    }
}

bool StorageManager::begin() {
    ready = false;
    usingSDMMC = false;
    littlefsReady = false;

#if defined(DISPLAY_WAVESHARE_349)
    // Try SD_MMC first on Waveshare 3.49
    Serial.println("[Storage] Attempting SD_MMC mount...");
    
    bool pinsSet = SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
    if (!pinsSet) {
        Serial.println("[Storage] SD_MMC.setPins() failed");
    } else if (SD_MMC.begin("/sdcard", true)) {  // 1-bit mode
        fs = &SD_MMC;
        ready = true;
        usingSDMMC = true;
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[Storage] SD card mounted (%lluMB)\n", cardSize);
        
        // Also mount LittleFS as secondary for backups
        // Use begin(false) to avoid auto-formatting existing data on transient errors
        littlefsReady = LittleFS.begin(false);
        if (!littlefsReady) {
            Serial.println("[Storage] WARNING: LittleFS secondary mount failed - mirror backups disabled");
            Serial.println("[Storage] (Run 'Format LittleFS' from maintenance if needed)");
        }

        return true;
    } else {
        Serial.println("[Storage] SD_MMC.begin() failed");
    }
#endif

    // Fallback to LittleFS
    Serial.println("[Storage] Trying LittleFS fallback...");
    if (LittleFS.begin(true)) {
        fs = &LittleFS;
        ready = true;
        littlefsReady = true;
        Serial.println("[Storage] LittleFS mounted");
        return true;
    }

    Serial.println("[Storage] All storage mount attempts failed!");
    return false;
}

String StorageManager::statusText() const {
    if (!ready) {
        return "No storage available";
    }
    if (usingSDMMC) {
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        return "SD card (" + String(cardSize) + "MB)";
    }
    return "LittleFS (internal)";
}

bool StorageManager::writeJsonFileAtomic(fs::FS& fs, const char* path, JsonDocument& doc) {
    // Ensure parent directory exists (prevents VFS fopen failures)
    if (path && path[0] == '/') {
        String parent(path);
        int slash = parent.lastIndexOf('/');
        if (slash > 0) {
            parent = parent.substring(0, slash);
            if (!fs.exists(parent)) {
                fs.mkdir(parent);
            }
        }
    }
    String tmpPath = String(path) + ".tmp";
    File tmp = fs.open(tmpPath.c_str(), "w");
    if (!tmp) {
        Serial.printf("[Storage] writeJsonFileAtomic: failed to open %s\n", tmpPath.c_str());
        return false;
    }
    size_t written = serializeJson(doc, tmp);
    tmp.flush();
    tmp.close();
    if (written == 0) {
        Serial.printf("[Storage] writeJsonFileAtomic: wrote 0 bytes to %s\n", tmpPath.c_str());
        fs.remove(tmpPath.c_str());
        return false;
    }
    // Only remove old file if it exists (avoids VFS error log spam)
    if (fs.exists(path)) {
        fs.remove(path);
    }
    if (!fs.rename(tmpPath.c_str(), path)) {
        Serial.printf("[Storage] writeJsonFileAtomic: rename failed %s -> %s\n", tmpPath.c_str(), path);
        // If rename fails, try to clean up
        fs.remove(tmpPath.c_str());
        return false;
    }
    return true;
}
