#include "camera_api_service.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstring>

#include "camera_runtime_module.h"
#include "camera_event_log.h"
#include "camera_index.h"
#include "../../storage_manager.h"
#include "../../settings_sanitize.h"

// Display preview helpers (defined in main.cpp).
extern void requestCameraPreviewCycleHold(uint32_t durationMs);
extern void requestCameraPreviewSingleHold(uint8_t cameraType, uint32_t durationMs, bool muted);
extern bool isDisplayPreviewRunning();
extern void cancelDisplayPreview();

namespace CameraApiService {

namespace {

uint8_t clampU8Value(int value, int minVal, int maxVal) {
    return clampU8(value, minVal, maxVal);
}

uint16_t clampU16Value(int value, int minVal, int maxVal) {
    return static_cast<uint16_t>(std::max(minVal, std::min(value, maxVal)));
}

struct VcamHeader {
    char magic[4];
    uint32_t version;
    uint32_t count;
    uint32_t recordSize;
};
static_assert(sizeof(VcamHeader) == 16, "VCAM header must be 16 bytes");

struct CameraCatalogDataset {
    bool present = false;
    bool valid = false;
    uint32_t count = 0;
    uint32_t bytes = 0;
};

CameraCatalogDataset readCameraCatalogDataset(fs::FS* fs, const char* path) {
    CameraCatalogDataset out;
    if (!fs || !path) {
        return out;
    }

    File file = fs->open(path, FILE_READ);
    if (!file) {
        return out;
    }
    out.present = true;
    out.bytes = static_cast<uint32_t>(file.size());

    VcamHeader header{};
    const size_t headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    file.close();
    if (headerRead != sizeof(header)) {
        return out;
    }

    const bool validHeader =
        std::memcmp(header.magic, "VCAM", 4) == 0 &&
        header.version == 1 &&
        header.recordSize == 24;
    if (!validHeader) {
        return out;
    }

    const uint32_t expectedBytes = static_cast<uint32_t>(sizeof(VcamHeader) + (header.count * header.recordSize));
    if (out.bytes < expectedBytes) {
        return out;
    }

    out.valid = true;
    out.count = header.count;
    return out;
}

}  // namespace

void sendStatus(WebServer& server,
                CameraRuntimeModule& cameraRuntimeModule) {
    const CameraRuntimeStatus runtimeStatus = cameraRuntimeModule.snapshot();
    const CameraIndexStats indexStats = cameraRuntimeModule.index().stats();
    // Runs from wifiManager.process() in loop(); direct eventLog snapshot reads are loop-context safe.
    const CameraEventLogStats eventStats = cameraRuntimeModule.eventLog().stats();

    auto lifecycleName = [](CameraLifecycleState state) -> const char* {
        switch (state) {
            case CameraLifecycleState::IDLE: return "IDLE";
            case CameraLifecycleState::ACTIVE: return "ACTIVE";
            case CameraLifecycleState::PREEMPTED: return "PREEMPTED";
            case CameraLifecycleState::SUPPRESSED_UNTIL_EXIT: return "SUPPRESSED_UNTIL_EXIT";
            default: return "UNKNOWN";
        }
    };
    auto clearReasonName = [](CameraClearReason reason) -> const char* {
        switch (reason) {
            case CameraClearReason::NONE: return "NONE";
            case CameraClearReason::PASS_DISTANCE: return "PASS_DISTANCE";
            case CameraClearReason::TURN_AWAY: return "TURN_AWAY";
            case CameraClearReason::ELIGIBILITY_INVALID: return "ELIGIBILITY_INVALID";
            case CameraClearReason::PREEMPTED_BY_SIGNAL: return "PREEMPTED_BY_SIGNAL";
            case CameraClearReason::REPLACED_BY_NEW_MATCH: return "REPLACED_BY_NEW_MATCH";
            default: return "UNKNOWN";
        }
    };

    JsonDocument doc;
    doc["success"] = true;
    doc["enabled"] = runtimeStatus.enabled;
    doc["indexLoaded"] = runtimeStatus.indexLoaded;
    doc["tickIntervalMs"] = runtimeStatus.tickIntervalMs;
    doc["lastTickMs"] = runtimeStatus.lastTickMs;
    doc["lastTickDurationUs"] = runtimeStatus.lastTickDurationUs;
    doc["maxTickDurationUs"] = runtimeStatus.maxTickDurationUs;
    doc["lastCandidatesChecked"] = runtimeStatus.lastCandidatesChecked;
    doc["lastMatches"] = runtimeStatus.lastMatches;
    doc["lastCapReached"] = runtimeStatus.lastCapReached;
    if (std::isfinite(runtimeStatus.lastHeadingDeltaDeg)) {
        doc["lastHeadingDeltaDeg"] = runtimeStatus.lastHeadingDeltaDeg;
    } else {
        doc["lastHeadingDeltaDeg"] = nullptr;
    }
    doc["lifecycleState"] = lifecycleName(runtimeStatus.lifecycleState);
    doc["lifecycleStateRaw"] = static_cast<uint8_t>(runtimeStatus.lifecycleState);
    doc["lastClearReason"] = clearReasonName(runtimeStatus.lastClearReason);
    doc["lastClearReasonRaw"] = static_cast<uint8_t>(runtimeStatus.lastClearReason);
    doc["suppressedCameraId"] = runtimeStatus.suppressedCameraId;
    doc["lastInternalFree"] = runtimeStatus.lastInternalFree;
    doc["lastInternalLargestBlock"] = runtimeStatus.lastInternalLargestBlock;
    doc["memoryGuardMinFree"] = runtimeStatus.memoryGuardMinFree;
    doc["memoryGuardMinLargestBlock"] = runtimeStatus.memoryGuardMinLargestBlock;

    JsonObject activeObj = doc["activeAlert"].to<JsonObject>();
    activeObj["active"] = runtimeStatus.activeAlert.active;
    activeObj["cameraId"] = runtimeStatus.activeAlert.cameraId;
    activeObj["type"] = runtimeStatus.activeAlert.type;
    activeObj["distanceM"] = runtimeStatus.activeAlert.distanceM;
    if (std::isfinite(runtimeStatus.activeAlert.headingDeltaDeg)) {
        activeObj["headingDeltaDeg"] = runtimeStatus.activeAlert.headingDeltaDeg;
    } else {
        activeObj["headingDeltaDeg"] = nullptr;
    }
    activeObj["startTsMs"] = runtimeStatus.activeAlert.startTsMs;
    activeObj["lastUpdateTsMs"] = runtimeStatus.activeAlert.lastUpdateTsMs;

    JsonObject counters = doc["counters"].to<JsonObject>();
    counters["cameraTicks"] = runtimeStatus.counters.cameraTicks;
    counters["cameraTickSkipsOverload"] = runtimeStatus.counters.cameraTickSkipsOverload;
    counters["cameraTickSkipsNonCore"] = runtimeStatus.counters.cameraTickSkipsNonCore;
    counters["cameraTickSkipsMemoryGuard"] = runtimeStatus.counters.cameraTickSkipsMemoryGuard;
    counters["cameraCandidatesChecked"] = runtimeStatus.counters.cameraCandidatesChecked;
    counters["cameraMatches"] = runtimeStatus.counters.cameraMatches;
    counters["cameraAlertsStarted"] = runtimeStatus.counters.cameraAlertsStarted;
    counters["cameraBudgetExceeded"] = runtimeStatus.counters.cameraBudgetExceeded;
    counters["cameraLoadFailures"] = runtimeStatus.loader.loadFailures;
    counters["cameraLoadSkipsMemoryGuard"] = runtimeStatus.loader.loadSkipsMemoryGuard;
    counters["cameraIndexSwapCount"] = runtimeStatus.counters.cameraIndexSwapCount;
    counters["cameraIndexSwapFailures"] = runtimeStatus.counters.cameraIndexSwapFailures;

    JsonObject loader = doc["loader"].to<JsonObject>();
    loader["loadAttempts"] = runtimeStatus.loader.loadAttempts;
    loader["loadFailures"] = runtimeStatus.loader.loadFailures;
    loader["loadSkipsMemoryGuard"] = runtimeStatus.loader.loadSkipsMemoryGuard;
    loader["lastAttemptMs"] = runtimeStatus.loader.lastAttemptMs;
    loader["lastSuccessMs"] = runtimeStatus.loader.lastSuccessMs;
    loader["lastLoadDurationMs"] = runtimeStatus.loader.lastLoadDurationMs;
    loader["maxLoadDurationMs"] = runtimeStatus.loader.maxLoadDurationMs;
    loader["lastSortDurationMs"] = runtimeStatus.loader.lastSortDurationMs;
    loader["lastSpanBuildDurationMs"] = runtimeStatus.loader.lastSpanBuildDurationMs;
    loader["lastInternalFree"] = runtimeStatus.loader.lastInternalFree;
    loader["lastInternalLargestBlock"] = runtimeStatus.loader.lastInternalLargestBlock;
    loader["memoryGuardMinFree"] = runtimeStatus.loader.memoryGuardMinFree;
    loader["memoryGuardMinLargestBlock"] = runtimeStatus.loader.memoryGuardMinLargestBlock;
    loader["taskRunning"] = runtimeStatus.loader.taskRunning;
    loader["loadInProgress"] = runtimeStatus.loader.loadInProgress;
    loader["reloadPending"] = runtimeStatus.loader.reloadPending;
    loader["readyVersion"] = runtimeStatus.loader.readyVersion;

    JsonObject indexObj = doc["index"].to<JsonObject>();
    indexObj["cameraCount"] = indexStats.cameraCount;
    indexObj["bucketCount"] = indexStats.bucketCount;
    indexObj["version"] = indexStats.version;

    JsonObject eventsObj = doc["events"].to<JsonObject>();
    eventsObj["published"] = eventStats.published;
    eventsObj["drops"] = eventStats.drops;
    eventsObj["size"] = static_cast<uint32_t>(eventStats.size);
    eventsObj["capacity"] = static_cast<uint32_t>(CameraEventLog::kCapacity);

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void sendCatalog(WebServer& server,
                 StorageManager& storageManager) {
    JsonDocument doc;
    doc["success"] = true;
    doc["storageReady"] = false;
    doc["tsMs"] = millis();

    JsonObject datasets = doc["datasets"].to<JsonObject>();
    JsonObject alprObj = datasets["alpr"].to<JsonObject>();
    JsonObject speedObj = datasets["speed"].to<JsonObject>();
    JsonObject redlightObj = datasets["redlight"].to<JsonObject>();

    auto writeDataset = [](JsonObject& obj, const CameraCatalogDataset& ds) {
        obj["present"] = ds.present;
        obj["valid"] = ds.valid;
        obj["count"] = ds.count;
        obj["bytes"] = ds.bytes;
    };

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        doc["message"] = "storage_unavailable";
        writeDataset(alprObj, {});
        writeDataset(speedObj, {});
        writeDataset(redlightObj, {});
        doc["totalCount"] = 0;
        doc["totalBytes"] = 0;
        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        doc["success"] = false;
        doc["message"] = "filesystem_unavailable";
        writeDataset(alprObj, {});
        writeDataset(speedObj, {});
        writeDataset(redlightObj, {});
        doc["totalCount"] = 0;
        doc["totalBytes"] = 0;
        String response;
        serializeJson(doc, response);
        server.send(500, "application/json", response);
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        doc["success"] = false;
        doc["message"] = "sd_busy";
        writeDataset(alprObj, {});
        writeDataset(speedObj, {});
        writeDataset(redlightObj, {});
        doc["totalCount"] = 0;
        doc["totalBytes"] = 0;
        String response;
        serializeJson(doc, response);
        server.send(503, "application/json", response);
        return;
    }

    const CameraCatalogDataset alpr = readCameraCatalogDataset(fs, "/alpr.bin");
    const CameraCatalogDataset speed = readCameraCatalogDataset(fs, "/speed_cam.bin");
    const CameraCatalogDataset redlight = readCameraCatalogDataset(fs, "/redlight_cam.bin");

    doc["storageReady"] = true;
    writeDataset(alprObj, alpr);
    writeDataset(speedObj, speed);
    writeDataset(redlightObj, redlight);
    doc["totalCount"] = alpr.count + speed.count + redlight.count;
    doc["totalBytes"] = alpr.bytes + speed.bytes + redlight.bytes;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void sendEvents(WebServer& server,
                CameraRuntimeModule& cameraRuntimeModule) {
    uint16_t limit = 16;
    if (server.hasArg("limit")) {
        limit = clampU16Value(server.arg("limit").toInt(), 1, static_cast<int>(CameraEventLog::kCapacity));
    }

    // Runs in loop()-owned WiFi manager context; eventLog() read access is safe here.
    CameraEvent recent[CameraEventLog::kCapacity] = {};
    const size_t count = cameraRuntimeModule.eventLog().copyRecent(recent, limit);
    const CameraEventLogStats stats = cameraRuntimeModule.eventLog().stats();

    JsonDocument doc;
    doc["success"] = true;
    doc["count"] = static_cast<uint32_t>(count);
    doc["published"] = stats.published;
    doc["drops"] = stats.drops;
    doc["size"] = static_cast<uint32_t>(stats.size);
    doc["capacity"] = static_cast<uint32_t>(CameraEventLog::kCapacity);

    JsonArray events = doc["events"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        const CameraEvent& sample = recent[i];
        JsonObject entry = events.add<JsonObject>();
        entry["tsMs"] = sample.tsMs;
        entry["cameraId"] = sample.cameraId;
        entry["distanceM"] = sample.distanceM;
        entry["type"] = sample.type;
        entry["synthetic"] = sample.synthetic;
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleDemo(WebServer& server) {
    uint8_t type = 0;
    if (server.hasArg("type")) {
        type = clampU8Value(server.arg("type").toInt(), 0, 4);
    }

    bool muted = false;
    if (server.hasArg("muted")) {
        String mutedArg = server.arg("muted");
        muted = mutedArg == "1" || mutedArg.equalsIgnoreCase("true") || mutedArg.equalsIgnoreCase("on");
    }

    uint16_t durationMs = 0;
    if (server.hasArg("durationMs")) {
        durationMs = clampU16Value(server.arg("durationMs").toInt(), 500, 15000);
    }

    // Demo requests always own preview mode; clear any active preview first.
    if (isDisplayPreviewRunning()) {
        cancelDisplayPreview();
    }

    if (type == 0) {
        if (durationMs == 0) {
            durationMs = 5400;
        }
        requestCameraPreviewCycleHold(durationMs);
    } else {
        if (durationMs == 0) {
            durationMs = 2200;
        }
        requestCameraPreviewSingleHold(type, durationMs, muted);
    }

    JsonDocument doc;
    doc["success"] = true;
    doc["active"] = true;
    doc["mode"] = "camera";
    doc["type"] = type;
    doc["muted"] = muted;
    doc["durationMs"] = durationMs;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleDemoClear(WebServer& server) {
    cancelDisplayPreview();
    server.send(200, "application/json", "{\"success\":true,\"active\":false}");
}

}  // namespace CameraApiService
