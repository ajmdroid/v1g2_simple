#include "debug_perf_files_service.h"

#include <algorithm>
#include <vector>
#include <esp_heap_caps.h>

#include "../../storage_manager.h"
#include "../../perf_sd_logger.h"
#include "json_stream_response.h"

namespace {

String fileNameFromPath(const String& path) {
    int slash = path.lastIndexOf('/');
    if (slash >= 0) {
        return path.substring(slash + 1);
    }
    return path;
}

bool isDigitsOnly(const String& text) {
    if (text.isEmpty()) {
        return false;
    }
    for (size_t i = 0; i < text.length(); ++i) {
        const char c = text.charAt(static_cast<unsigned>(i));
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool parseLegacyBootId(const String& name, uint32_t& outBootId) {
    if (!name.startsWith("perf_boot_") || !name.endsWith(".csv")) {
        return false;
    }
    const int digitStart = strlen("perf_boot_");
    const int digitEnd = name.length() - 4;  // Exclude ".csv"
    if (digitEnd <= digitStart) {
        return false;
    }
    String digits = name.substring(digitStart, digitEnd);
    if (!isDigitsOnly(digits)) {
        return false;
    }
    outBootId = static_cast<uint32_t>(strtoul(digits.c_str(), nullptr, 10));
    return true;
}

bool parseTimestampedBootId(const String& name, uint32_t& outBootId) {
    if (!name.endsWith(".csv")) {
        return false;
    }

    // Expected: YYYYMMDD_HHMMSS_perf_<bootId>.csv
    const int perfTokenPos = name.indexOf("_perf_");
    if (perfTokenPos <= 0) {
        return false;
    }

    const String timestamp = name.substring(0, perfTokenPos);
    if (timestamp.length() != 15 || timestamp.charAt(8) != '_') {
        return false;
    }
    const String datePart = timestamp.substring(0, 8);
    const String timePart = timestamp.substring(9);
    if (!isDigitsOnly(datePart) || !isDigitsOnly(timePart)) {
        return false;
    }

    const int idStart = perfTokenPos + strlen("_perf_");
    const int idEnd = name.length() - 4;  // Exclude ".csv"
    if (idEnd <= idStart) {
        return false;
    }
    const String bootDigits = name.substring(idStart, idEnd);
    if (!isDigitsOnly(bootDigits)) {
        return false;
    }
    outBootId = static_cast<uint32_t>(strtoul(bootDigits.c_str(), nullptr, 10));
    return true;
}

bool parsePerfBootId(const String& name, uint32_t& outBootId) {
    if (parseLegacyBootId(name, outBootId)) {
        return true;
    }
    if (parseTimestampedBootId(name, outBootId)) {
        return true;
    }
    return false;
}

bool isValidPerfFileName(const String& name) {
    if (name.length() == 0 || name.length() > 64) {
        return false;
    }
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) {
        return false;
    }
    if (name == "perf.csv") {
        return true;  // Legacy fallback filename.
    }
    uint32_t bootId = 0;
    return parsePerfBootId(name, bootId);
}

bool perfFilePathFromName(const String& name, String& outPath) {
    if (!isValidPerfFileName(name)) {
        return false;
    }
    outPath = "/perf/" + name;
    return true;
}

struct PerfFileInfo {
    String name;
    uint32_t sizeBytes;
    uint32_t bootId;
};

struct PerfFileListCache {
    bool valid = false;
    uint32_t builtAtMs = 0;
    std::vector<PerfFileInfo> rows;
};

constexpr uint32_t PERF_FILES_CACHE_TTL_MS = 2500;
PerfFileListCache gPerfFileListCache;

uint16_t clampPerfFilesLimit(int value) {
    if (value < 1) return 1;
    if (value > 64) return 64;
    return static_cast<uint16_t>(value);
}

uint16_t perfFilesLimitFromRequest(WebServer& server) {
    if (!server.hasArg("limit")) return 24;
    return clampPerfFilesLimit(server.arg("limit").toInt());
}

bool isPerfFileCacheFresh() {
    if (!gPerfFileListCache.valid) return false;
    const uint32_t now = millis();
    return (now - gPerfFileListCache.builtAtMs) <= PERF_FILES_CACHE_TTL_MS;
}

void invalidatePerfFileCache() {
    gPerfFileListCache.valid = false;
    gPerfFileListCache.builtAtMs = 0;
    gPerfFileListCache.rows.clear();
}

void sendPerfFilesList(WebServer& server) {
    const uint16_t limit = perfFilesLimitFromRequest(server);
    JsonDocument doc;
    doc["success"] = true;
    doc["storageReady"] = storageManager.isReady();
    doc["onSdCard"] = storageManager.isSDCard();
    doc["path"] = "/perf";
    doc["limit"] = limit;

    JsonArray filesArr = doc["files"].to<JsonArray>();

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        invalidatePerfFileCache();
        sendJsonStream(server, doc);
        return;
    }

    if (!isPerfFileCacheFresh()) {
        StorageManager::SDTryLock lock(storageManager.getSDMutex());
        if (!lock) {
            doc["success"] = false;
            doc["error"] = lock.isDmaStarved() ? "Low DMA heap; perf file listing deferred" : "SD busy";
            sendJsonStream(server, doc, 503);
            return;
        }

        fs::FS* fs = storageManager.getFilesystem();
        if (!fs || !fs->exists("/perf")) {
            invalidatePerfFileCache();
            sendJsonStream(server, doc);
            return;
        }

        File dir = fs->open("/perf");
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            doc["success"] = false;
            doc["error"] = "Failed to open /perf directory";
            sendJsonStream(server, doc, 500);
            return;
        }

        std::vector<PerfFileInfo> rows;
        rows.reserve(16);

        File entry;
        while ((entry = dir.openNextFile())) {
            if (entry.isDirectory()) {
                entry.close();
                continue;
            }

            String name = fileNameFromPath(entry.name());
            if (!isValidPerfFileName(name)) {
                entry.close();
                continue;
            }

            uint32_t bootId = 0;
            (void)parsePerfBootId(name, bootId);

            rows.push_back({name, static_cast<uint32_t>(entry.size()), bootId});
            entry.close();
        }
        dir.close();

        std::sort(rows.begin(), rows.end(), [](const PerfFileInfo& a, const PerfFileInfo& b) {
            if (a.bootId != b.bootId) {
                return a.bootId > b.bootId;
            }
            return a.name > b.name;
        });

        gPerfFileListCache.rows = std::move(rows);
        gPerfFileListCache.valid = true;
        gPerfFileListCache.builtAtMs = millis();
    }

    const size_t countTotal = gPerfFileListCache.rows.size();
    const size_t countReturned = std::min(countTotal, static_cast<size_t>(limit));
    const String activePath = String(perfSdLogger.csvPath());
    for (size_t i = 0; i < countReturned; ++i) {
        const PerfFileInfo& row = gPerfFileListCache.rows[i];
        JsonObject f = filesArr.add<JsonObject>();
        f["name"] = row.name;
        f["sizeBytes"] = row.sizeBytes;
        f["bootId"] = row.bootId;
        f["active"] = (String("/perf/") + row.name) == activePath;
    }
    doc["count"] = static_cast<uint32_t>(countTotal);
    doc["countReturned"] = static_cast<uint32_t>(countReturned);
    doc["cacheAgeMs"] = isPerfFileCacheFresh()
                            ? (millis() - gPerfFileListCache.builtAtMs)
                            : static_cast<uint32_t>(0);

    sendJsonStream(server, doc);
}

void handlePerfFileDownload(WebServer& server) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
        return;
    }

    String requestedName = server.arg("name");
    String path;
    if (!perfFilePathFromName(requestedName, path)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
        return;
    }

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD storage unavailable\"}");
        return;
    }

    if (perfSdLogger.isEnabled()) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"Perf logging active; download unavailable\"}");
        return;
    }

    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock) {
        if (lock.isDmaStarved()) {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"Low DMA heap; try again\"}");
        } else {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"SD busy\"}");
        }
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(path)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    File f = fs->open(path, FILE_READ);
    if (!f) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to open file\"}");
        return;
    }

    static constexpr size_t CHUNK_SIZE = 4096;
    // File-download buffer in PSRAM — saves 4 KiB internal .bss.
    // VFS reads use an internal bounce buffer; lwIP copies for TX.
    static uint8_t* buffer = nullptr;
    if (!buffer) {
        buffer = static_cast<uint8_t*>(
            heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    }
    if (!buffer) {
        f.close();
        server.send(500, "application/json",
                    "{\"success\":false,\"error\":\"PSRAM alloc failed\"}");
        return;
    }

    size_t fileSize = f.size();
    server.sendHeader("Content-Type", "text/csv");
    String contentDisposition = String("attachment; filename=\"") + requestedName + "\"";
    server.sendHeader("Content-Disposition", contentDisposition);
    server.sendHeader("Cache-Control", "no-cache");
    server.setContentLength(fileSize);
    server.send(200, "text/csv", "");

    size_t totalSent = 0;
    while (f.available() && server.client().connected()) {
        size_t toRead = std::min(CHUNK_SIZE, static_cast<size_t>(f.available()));
        size_t bytesRead = f.read(buffer, toRead);
        if (bytesRead > 0) {
            server.client().write(buffer, bytesRead);
            totalSent += bytesRead;
        }
        yield();
        if ((totalSent % 32768) == 0) {
            delay(1);
        }
    }

    f.close();
}

void handlePerfFileDelete(WebServer& server) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
        return;
    }

    String requestedName = server.arg("name");
    String path;
    if (!perfFilePathFromName(requestedName, path)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
        return;
    }

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD storage unavailable\"}");
        return;
    }

    if (perfSdLogger.isEnabled()) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"Perf logging active; delete unavailable\"}");
        return;
    }

    StorageManager::SDTryLock lock(storageManager.getSDMutex());
    if (!lock) {
        if (lock.isDmaStarved()) {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"Low DMA heap; try again\"}");
        } else {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"SD busy\"}");
        }
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(path)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    bool ok = fs->remove(path);
    if (ok) {
        invalidatePerfFileCache();
    }

    JsonDocument doc;
    doc["success"] = ok;
    doc["name"] = requestedName;
    if (!ok) {
        doc["error"] = "Delete failed";
    }

    sendJsonStream(server, doc, ok ? 200 : 500);
}

}  // namespace

namespace DebugPerfFilesService {

void handleApiPerfFilesList(WebServer& server,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendPerfFilesList(server);
}

void handleApiPerfFilesDownload(WebServer& server,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDownload(server);
}

void handleApiPerfFilesDelete(WebServer& server,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDelete(server);
}

}  // namespace DebugPerfFilesService
