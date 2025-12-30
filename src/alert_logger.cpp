#include "alert_logger.h"
#include <SD.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <vector>

AlertLogger alertLogger;

namespace {
String boolTo01(bool v) {
    return v ? "1" : "0";
}
} // namespace

AlertLogger::AlertLogger()
    : fs(nullptr), ready(false), usingSDMMC(false), logPath(ALERT_LOG_PATH) {
    lastSnapshot = Snapshot();
}

bool AlertLogger::begin() {
    ready = false;
    usingSDMMC = false;

#if SD_LOGGER_USE_SD_MMC
    if (!ready) {
        Serial.println("AlertLogger: Attempting to mount SD card...");
        Serial.printf("AlertLogger: Pin values - CLK:%d CMD:%d D0:%d\n", 
                     SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
        
#ifdef TFT_D0
        Serial.printf("WARNING: TFT_D0 is defined as %d - THIS CONFLICTS WITH SD_MMC!\n", TFT_D0);
#endif
#ifdef TFT_D1
        Serial.printf("WARNING: TFT_D1 is defined as %d - THIS CONFLICTS WITH SD_MMC!\n", TFT_D1);
#endif
#ifdef TFT_D2
        Serial.printf("WARNING: TFT_D2 is defined as %d - THIS CONFLICTS WITH SD_MMC!\n", TFT_D2);
#endif
        
        // setPins() must be called BEFORE begin()
        // For ESP32-S3, setPins returns true if successful
        Serial.println("AlertLogger: Calling SD_MMC.setPins()...");
        bool pinsSet = SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
        Serial.printf("AlertLogger: SD_MMC.setPins() returned: %s\n", pinsSet ? "true" : "false");
        
        if (!pinsSet) {
            Serial.println("AlertLogger: ERROR - Failed to set SD_MMC pins!");
            Serial.println("  Likely cause: Pin conflict with TFT parallel interface");
            Serial.println("  Solution: These pins can't be used for both SD and display");
            return false;
        }
        
        // Try mounting with 1-bit mode (true), no format on fail
        Serial.println("AlertLogger: Calling SD_MMC.begin('/sdcard', true) for 1-bit mode...");
        if (SD_MMC.begin("/sdcard", true)) {
            fs = &SD_MMC;
            ready = true;
            usingSDMMC = true;
            uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
            Serial.printf("AlertLogger: SUCCESS - SD card mounted (Size: %lluMB, Type: %s)\n", 
                         cardSize, 
                         SD_MMC.cardType() == CARD_SD ? "SDSC" : 
                         SD_MMC.cardType() == CARD_SDHC ? "SDHC" : "Unknown");
        } else {
            Serial.println("AlertLogger: ERROR - SD_MMC.begin() failed!");
            Serial.println("  Possible causes:");
            Serial.println("  - SD card not inserted");
            Serial.println("  - SD card not formatted as FAT32");
            Serial.println("  - SD card hardware issue");
            Serial.println("  - Pin conflicts with display");
        }
    }
#endif

#if SD_LOGGER_USE_SPI
    if (!ready) {
        SPI.begin(SD_CARD_SCK, SD_CARD_MISO, SD_CARD_MOSI, SD_CARD_CS);
        if (SD.begin(SD_CARD_CS, SPI, SD_CARD_FREQ)) {
            fs = &SD;
            ready = true;
            Serial.println("AlertLogger: mounted SPI SD card");
        } else {
            Serial.println("AlertLogger: SPI SD mount failed");
        }
    }
#endif

    if (!ready) {
        Serial.println("AlertLogger: SD not available, logging disabled");
        return false;
    }

    // Create file with header if missing
    if (!fs->exists(logPath)) {
        File file = fs->open(logPath, FILE_WRITE);
        if (file) {
            file.println("ms,event,band,freq,dir,front,rear,count,muted");
            file.close();
        }
    }

    return true;
}

String AlertLogger::statusText() const {
    if (!ready) {
        return "SD not mounted";
    }
    return usingSDMMC ? "SD_MMC mounted" : "SPI SD mounted";
}

AlertLogger::Snapshot AlertLogger::makeSnapshot(const AlertData& alert, const DisplayState& state, size_t count) const {
    Snapshot snap;
    snap.active = alert.isValid && alert.band != BAND_NONE;
    snap.band = alert.band;
    snap.direction = alert.direction;
    snap.frequency = alert.frequency;
    snap.front = alert.frontStrength;
    snap.rear = alert.rearStrength;
    snap.count = count;
    snap.muted = state.muted;
    return snap;
}

bool AlertLogger::shouldLog(const Snapshot& snap) const {
    if (snap.active != lastSnapshot.active) {
        return true;
    }
    if (!snap.active && !lastSnapshot.active) {
        return false; // Already logged clear state
    }

    return snap.band != lastSnapshot.band ||
           snap.direction != lastSnapshot.direction ||
           snap.frequency != lastSnapshot.frequency ||
           snap.front != lastSnapshot.front ||
           snap.rear != lastSnapshot.rear ||
           snap.count != lastSnapshot.count ||
           snap.muted != lastSnapshot.muted;
}

String AlertLogger::bandToString(Band band) const {
    switch (band) {
        case BAND_KA: return "Ka";
        case BAND_K:  return "K";
        case BAND_X:  return "X";
        case BAND_LASER: return "LASER";
        default: return "NONE";
    }
}

String AlertLogger::dirToString(Direction dir) const {
    switch (dir) {
        case DIR_FRONT: return "FRONT";
        case DIR_SIDE:  return "SIDE";
        case DIR_REAR:  return "REAR";
        default: return "NONE";
    }
}

String AlertLogger::formatLine(const Snapshot& snap, unsigned long ts) const {
    String line;
    line.reserve(80);
    line += ts;
    line += snap.active ? ",ALERT," : ",CLEAR,";
    line += bandToString(snap.band);
    line += ",";
    line += snap.frequency;
    line += ",";
    line += dirToString(snap.direction);
    line += ",";
    line += snap.front;
    line += ",";
    line += snap.rear;
    line += ",";
    line += snap.count;
    line += ",";
    line += boolTo01(snap.muted);
    line += "\n";
    return line;
}

bool AlertLogger::appendLine(const String& line) const {
    if (!ready || !fs) {
        return false;
    }

    File file = fs->open(logPath, FILE_APPEND);
    if (!file) {
        // Fallback to write if append unavailable
        file = fs->open(logPath, FILE_WRITE);
    }

    if (!file) {
        Serial.println("AlertLogger: failed to open log file for append");
        return false;
    }

    size_t written = file.print(line);
    file.close();
    return written == line.length();
}

bool AlertLogger::logAlert(const AlertData& alert, const DisplayState& state, size_t alertCount) {
    if (!ready) {
        return false;
    }

    Snapshot snap = makeSnapshot(alert, state, alertCount);
    if (!shouldLog(snap)) {
        return false;
    }

    String line = formatLine(snap, millis());
    bool ok = appendLine(line);
    if (ok) {
        lastSnapshot = snap;
    }
    return ok;
}

bool AlertLogger::updateStateOnClear(const DisplayState& state) {
    // Don't log CLEAR/NONE events - just update internal state
    if (!ready) {
        return false;
    }

    AlertData none;
    none.isValid = true;
    none.band = BAND_NONE;
    none.direction = DIR_NONE;
    none.frequency = 0;
    none.frontStrength = 0;
    none.rearStrength = 0;
    Snapshot snap = makeSnapshot(none, state, 0);
    snap.active = false;

    // Update state so next real alert gets logged
    lastSnapshot = snap;
    return true;
}

bool AlertLogger::parseLine(const String& line, String parts[], size_t expected) const {
    int start = 0;
    size_t idx = 0;
    while (idx < expected) {
        int sep = line.indexOf(',', start);
        if (sep == -1) {
            parts[idx++] = line.substring(start);
            break;
        }
        parts[idx++] = line.substring(start, sep);
        start = sep + 1;
    }
    return idx == expected;
}

String AlertLogger::getRecentJson(size_t maxLines) const {
    if (!ready || !fs) {
        return "[]";
    }

    File file = fs->open(logPath, FILE_READ);
    if (!file) {
        return "[]";
    }

    std::vector<String> lines;
    lines.reserve(maxLines);

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("ms,")) {
            continue; // skip header/blank
        }
        lines.push_back(line);
        if (lines.size() > maxLines) {
            lines.erase(lines.begin());
        }
    }
    file.close();

    String json = "[";
    bool first = true;
    for (const auto& l : lines) {
        String cols[9];
        if (!parseLine(l, cols, 9)) {
            continue;
        }

        if (!first) {
            json += ",";
        }
        first = false;

        json += "{\"ms\":";
        json += cols[0];
        json += ",\"event\":\"";
        json += cols[1];
        json += "\",\"band\":\"";
        json += cols[2];
        json += "\",\"freq\":";
        json += cols[3];
        json += ",\"direction\":\"";
        json += cols[4];
        json += "\",\"front\":";
        json += cols[5];
        json += ",\"rear\":";
        json += cols[6];
        json += ",\"count\":";
        json += cols[7];
        json += ",\"muted\":";
        json += ((cols[8] == "1" || cols[8] == "true") ? "true" : "false");
        json += "}";
    }
    json += "]";
    return json;
}

bool AlertLogger::clear() {
    if (!ready || !fs) {
        return false;
    }

    bool removed = fs->remove(logPath);
    File file = fs->open(logPath, FILE_WRITE);
    bool created = false;
    if (file) {
        file.println("ms,event,band,freq,dir,front,rear,count,muted");
        file.close();
        created = true;
    }
    lastSnapshot = Snapshot();
    return removed || created;
}

void AlertLogger::setTimestampUTC(uint32_t unixTime) {
    timestampUTC = unixTime;
}
