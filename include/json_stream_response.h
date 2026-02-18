#pragma once
/**
 * Streaming JSON helper — sends JSON directly to the WiFi client
 * without allocating an intermediate Arduino String.
 *
 * Saves ~3-15 KB of internal SRAM per API response depending on
 * document size. Avoids heap fragmentation from short-lived String
 * allocations during WiFi request handling.
 *
 * Usage:
 *   JsonDocument doc;
 *   doc["key"] = "value";
 *   sendJsonStream(server, doc);          // 200 OK
 *   sendJsonStream(server, doc, 400);     // custom status code
 */

#include <ArduinoJson.h>
#include <WebServer.h>

inline void sendJsonStream(WebServer& server, JsonDocument& doc, int code = 200) {
    const size_t len = measureJson(doc);
    server.setContentLength(len);
    server.send(code, "application/json", "");
    serializeJson(doc, server.client());
}
