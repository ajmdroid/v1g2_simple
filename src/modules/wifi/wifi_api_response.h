#ifndef WIFI_API_RESPONSE_H
#define WIFI_API_RESPONSE_H

#include <ArduinoJson.h>
#include <WebServer.h>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiApiResponse {

inline void sendJsonDocument(WebServer& server, int statusCode, const JsonDocument& doc) {
#ifdef UNIT_TEST
    std::string response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response.c_str());
#else
    String response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response);
#endif
}

inline void setErrorAndMessage(JsonDocument& doc, const char* text) {
    const char* value = text ? text : "Unknown error";
    doc["error"] = value;
    doc["message"] = value;
}

}  // namespace WifiApiResponse

#endif  // WIFI_API_RESPONSE_H
