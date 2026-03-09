/**
 * WiFi manager support helpers split out of wifi_manager.cpp.
 *
 * Keeps filesystem/static-file serving and API-facing enum string helpers
 * separate from the main runtime/orchestration implementation.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include <LittleFS.h>

void dumpLittleFSRoot() {
    if (!LittleFS.begin(true, "/littlefs", 10, "storage")) {
        Serial.println("[SetupMode] ERROR: Failed to mount LittleFS for root dump");
        return;
    }

    Serial.println("[SetupMode] Dumping LittleFS root...");
    Serial.println("[SetupMode] Files in LittleFS root:");

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("[SetupMode] ERROR: Could not open root directory");
        if (root) root.close();
        return;
    }

    File file = root.openNextFile();
    bool hasFiles = false;
    while (file) {
        hasFiles = true;
        Serial.printf("[SetupMode]   %s (%u bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }

    if (!hasFiles) {
        Serial.println("[SetupMode]   (empty)");
    }

    root.close();
}

const char* wifiClientStateApiName(WifiClientState state) {
    switch (state) {
        case WIFI_CLIENT_DISABLED: return "disabled";
        case WIFI_CLIENT_DISCONNECTED: return "disconnected";
        case WIFI_CLIENT_CONNECTING: return "connecting";
        case WIFI_CLIENT_CONNECTED: return "connected";
        case WIFI_CLIENT_FAILED: return "failed";
        default: return "unknown";
    }
}

bool serveLittleFSFileHelper(WebServer& server, const char* path, const char* contentType) {
    uint32_t startUs = PERF_TIMESTAMP_US();
    String acceptEncoding = server.header("Accept-Encoding");
    bool clientAcceptsGzip = acceptEncoding.indexOf("gzip") >= 0;

    if (clientAcceptsGzip) {
        String gzPath = String(path) + ".gz";
        if (LittleFS.exists(gzPath.c_str())) {
            File file = LittleFS.open(gzPath.c_str(), "r");
            if (file) {
                size_t fileSize = file.size();
                String etag = String("\"") + String(path) + ".gz-" + String(fileSize) + String("\"");
                if (server.header("If-None-Match") == etag) {
                    server.sendHeader("ETag", etag);
                    server.send(304, contentType, "");
                    file.close();
                    return true;
                }
                server.setContentLength(fileSize);
                server.sendHeader("Content-Encoding", "gzip");
                server.sendHeader("Cache-Control", "max-age=86400");
                server.sendHeader("ETag", etag);
                server.send(200, contentType, "");
                Serial.printf("[HTTP] 200 %s -> %s.gz (%u bytes)\n", path, path, fileSize);

                // Stream file content.
                uint8_t buf[1024];
                while (file.available()) {
                    size_t len = file.read(buf, sizeof(buf));
                    server.client().write(buf, len);
                    yield();  // Allow FreeRTOS to schedule other tasks (BLE queue drain).
                }
                file.close();
                perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
                return true;
            }
        }
    }

    File file = LittleFS.open(path, "r");
    if (!file) {
        Serial.printf("[HTTP] MISS %s (file not found)\n", path);
        return false;
    }
    size_t fileSize = file.size();
    String etag = String("\"") + String(path) + "-" + String(fileSize) + String("\"");
    if (server.header("If-None-Match") == etag) {
        server.sendHeader("ETag", etag);
        server.send(304, contentType, "");
        file.close();
        return true;
    }
    server.sendHeader("Cache-Control", "max-age=86400");
    server.sendHeader("ETag", etag);
    Serial.printf("[HTTP] 200 %s (%u bytes)\n", path, fileSize);
    server.streamFile(file, contentType);
    file.close();
    perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
    return true;
}
