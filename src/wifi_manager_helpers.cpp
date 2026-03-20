/**
 * WiFi manager support helpers split out of wifi_manager.cpp.
 *
 * Keeps filesystem/static-file serving and API-facing enum string helpers
 * separate from the main runtime/orchestration implementation.
 */

#include "wifi_manager_internals.h"
#include "client_write_retry.h"
#include "perf_metrics.h"
#include <LittleFS.h>

void dumpLittleFSRoot() {
    if (!LittleFS.begin(false, "/littlefs", 10, "storage")) {
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

namespace {

bool streamOpenFile(WebServer& server,
                    File& file,
                    const char* path,
                    const char* contentType,
                    size_t fileSize,
                    bool gzip) {
    constexpr size_t kStreamChunkBytes = 256;
    server.setContentLength(fileSize);
    if (gzip) {
        server.sendHeader("Content-Encoding", "gzip");
    }
    server.sendHeader("Cache-Control", "max-age=86400");
    server.sendHeader("ETag",
                      String("\"") + String(path) + (gzip ? ".gz-" : "-") + String(fileSize) + String("\""));
    server.send(200, contentType, "");

    auto client = server.client();
    // WiFi static-file serving is Tier 5. Prefer smaller loopTask chunks over
    // a large transient stack buffer on the WebServer path.
    uint8_t buf[kStreamChunkBytes];
    size_t bytesSent = 0;
    while (file.available()) {
        const size_t len = file.read(buf, sizeof(buf));
        if (len == 0) {
            break;
        }
        if (!client_write_retry::writeAll(client, buf, len)) {
            client.stop();  // Fail-fast on short/partial stream writes.
            Serial.printf("[HTTP] WARN stream failed %s (%u/%u bytes)\n",
                          path,
                          static_cast<unsigned>(bytesSent),
                          static_cast<unsigned>(fileSize));
            return false;
        }
        bytesSent += len;
    }

    if (bytesSent != fileSize) {
        client.stop();
        Serial.printf("[HTTP] WARN stream short %s (%u/%u bytes)\n",
                      path,
                      static_cast<unsigned>(bytesSent),
                      static_cast<unsigned>(fileSize));
        return false;
    }

    return true;
}

}  // namespace

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
                Serial.printf("[HTTP] 200 %s -> %s.gz (%u bytes)\n", path, path, fileSize);
                const bool streamOk = streamOpenFile(server, file, path, contentType, fileSize, true);
                file.close();
                if (!streamOk) {
                    return true;
                }
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
    Serial.printf("[HTTP] 200 %s (%u bytes)\n", path, fileSize);
    const bool streamOk = streamOpenFile(server, file, path, contentType, fileSize, false);
    file.close();
    if (!streamOk) {
        return true;
    }
    perfRecordFsServeUs(PERF_TIMESTAMP_US() - startUs);
    return true;
}
