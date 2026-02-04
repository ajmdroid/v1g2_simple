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
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
    bool isLittleFSReady() const { return littlefsReady; }
    String statusText() const;
    
    // Get underlying filesystem
    fs::FS* getFilesystem() const { return fs; }
    // Secondary LittleFS handle (available even when SD is primary)
    fs::FS* getLittleFS() const { return littlefsReady ? &LittleFS : nullptr; }
    
    // Thread-safe SD access mutex - MUST be held during all file operations
    // when multiple cores/tasks may access SD simultaneously
    SemaphoreHandle_t getSDMutex() const { return sdMutex; }
    
    // RAII helper for automatic mutex acquisition
    class SDLock {
    public:
        explicit SDLock(SemaphoreHandle_t mutex, TickType_t timeout = pdMS_TO_TICKS(1000)) 
            : mutex_(mutex), acquired_(false) {
            if (mutex_) {
                acquired_ = (xSemaphoreTake(mutex_, timeout) == pdTRUE);
            }
        }
        ~SDLock() { release(); }
        bool acquired() const { return acquired_; }
        operator bool() const { return acquired_; }
        
        // Manual early release - useful when you want to release lock before scope ends
        void release() {
            if (acquired_ && mutex_) {
                xSemaphoreGive(mutex_);
                acquired_ = false;
            }
        }
    private:
        SemaphoreHandle_t mutex_;
        bool acquired_;
    };
    
    // Camera database info
    bool hasCameraDatabase() const { return cameraDbFound; }
    uint32_t getAlprCount() const { return alprCount; }
    uint32_t getRedlightCount() const { return redlightCount; }
    uint32_t getSpeedCount() const { return speedCount; }
    
    // Atomic JSON file write utility (write to .tmp, then rename)
    // Returns true on success. Used by lockout and auto-lockout managers.
    static bool writeJsonFileAtomic(fs::FS& fs, const char* path, JsonDocument& doc);

private:
    void checkCameraDatabase();
    uint32_t countJsonLines(const char* path);
    
    fs::FS* fs;
    bool ready;
    bool usingSDMMC;
    bool littlefsReady;
    bool cameraDbFound;
    uint32_t alprCount;
    uint32_t redlightCount;
    uint32_t speedCount;
    SemaphoreHandle_t sdMutex;
};

// Global instance
extern StorageManager storageManager;

#endif // STORAGE_MANAGER_H
