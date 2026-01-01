/**
 * Storage Manager - SD card and LittleFS mounting
 * 
 * Provides filesystem access for profiles, web files, and caching.
 * Alert logging has been removed - this is just storage management.
 */

#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <Arduino.h>
#include <FS.h>

// Waveshare 3.49 SD card pins (SDMMC interface)
#if defined(DISPLAY_WAVESHARE_349)
    #ifndef SD_MMC_CLK_PIN
    #define SD_MMC_CLK_PIN 41
    #endif
    #ifndef SD_MMC_CMD_PIN
    #define SD_MMC_CMD_PIN 39
    #endif
    #ifndef SD_MMC_D0_PIN
    #define SD_MMC_D0_PIN 40
    #endif
#else
    #ifndef SD_MMC_CLK_PIN
    #define SD_MMC_CLK_PIN -1
    #endif
    #ifndef SD_MMC_CMD_PIN
    #define SD_MMC_CMD_PIN -1
    #endif
    #ifndef SD_MMC_D0_PIN
    #define SD_MMC_D0_PIN -1
    #endif
#endif

class StorageManager {
public:
    StorageManager();

    // Mount storage (SD card preferred, LittleFS fallback)
    bool begin();

    bool isReady() const { return ready; }
    bool isSDCard() const { return usingSDMMC; }
    String statusText() const;
    
    // Get underlying filesystem
    fs::FS* getFilesystem() const { return fs; }

private:
    fs::FS* fs;
    bool ready;
    bool usingSDMMC;
};

// Global instance
extern StorageManager storageManager;

#endif // STORAGE_MANAGER_H
