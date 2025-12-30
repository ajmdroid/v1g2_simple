#ifndef ALERT_LOGGER_H
#define ALERT_LOGGER_H

#include <Arduino.h>
#include <FS.h>
#include "packet_parser.h"

// Logging configuration (override via build flags if needed)
#ifndef SD_LOGGER_USE_SD_MMC
#define SD_LOGGER_USE_SD_MMC 1
#endif
#ifndef SD_LOGGER_USE_SPI
#define SD_LOGGER_USE_SPI 0
#endif

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
    // Default pins (disabled for other boards)
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
#ifndef SD_CARD_CS
#define SD_CARD_CS 10
#endif
#ifndef SD_CARD_SCK
#define SD_CARD_SCK 12
#endif
#ifndef SD_CARD_MOSI
#define SD_CARD_MOSI 11
#endif
#ifndef SD_CARD_MISO
#define SD_CARD_MISO 13
#endif
#ifndef SD_CARD_FREQ
#define SD_CARD_FREQ 16000000
#endif
#ifndef ALERT_LOG_PATH
#define ALERT_LOG_PATH "/alerts.csv"
#endif
#ifndef ALERT_LOG_MAX_RECENT
#define ALERT_LOG_MAX_RECENT 200
#endif

class AlertLogger {
public:
    AlertLogger();

    // Mount the SD card and ensure the log file exists
    bool begin();

    bool isReady() const { return ready; }
    String statusText() const;
    
    // Get underlying filesystem for other components
    fs::FS* getFilesystem() const { return fs; }

    // Record alert transitions (deduplicated internally)
    bool logAlert(const AlertData& alert, const DisplayState& state, size_t alertCount);
    bool updateStateOnClear(const DisplayState& state);

    // Read back recent log entries as JSON (newest order preserved)
    String getRecentJson(size_t maxLines = ALERT_LOG_MAX_RECENT) const;

    // Set the UTC timestamp from NTP
    void setTimestampUTC(uint32_t unixTime);

    // Remove the log file and recreate with header
    bool clear();

private:
    struct Snapshot {
        bool active = false;
        Band band = BAND_NONE;
        Direction direction = DIR_NONE;
        uint32_t frequency = 0;
        uint8_t front = 0;
        uint8_t rear = 0;
        size_t count = 0;
        bool muted = false;
    };

    fs::FS* fs;
    bool ready;
    bool usingSDMMC;
    String logPath;
    Snapshot lastSnapshot;
    uint32_t timestampUTC;

    bool appendLine(const String& line) const;
    Snapshot makeSnapshot(const AlertData& alert, const DisplayState& state, size_t count) const;
    bool shouldLog(const Snapshot& snap) const;
    String formatLine(const Snapshot& snap, unsigned long ts) const;
    String bandToString(Band band) const;
    String dirToString(Direction dir) const;
    bool parseLine(const String& line, String parts[], size_t expected) const;
};

extern AlertLogger alertLogger;

#endif // ALERT_LOGGER_H
