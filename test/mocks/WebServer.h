#pragma once

#include <string>
#include <unordered_map>

#include "Arduino.h"

class WebServer {
public:
    explicit WebServer(int /*port*/ = 80) {}

    bool hasArg(const String& name) const {
        return args_.find(name.c_str()) != args_.end();
    }

    String arg(const String& name) const {
        auto it = args_.find(name.c_str());
        if (it == args_.end()) {
            return "";
        }
        return it->second.c_str();
    }

    String arg(const char* name) const {
        return arg(String(name));
    }

    void setArg(const String& name, const String& value) {
        args_[name.c_str()] = value.c_str();
    }

    void clearArgs() {
        args_.clear();
    }

    bool hasHeader(const String& name) const {
        return requestHeaders_.find(name.c_str()) != requestHeaders_.end();
    }

    String header(const String& name) const {
        auto it = requestHeaders_.find(name.c_str());
        if (it == requestHeaders_.end()) {
            return "";
        }
        return it->second.c_str();
    }

    void setHeader(const String& name, const String& value) {
        requestHeaders_[name.c_str()] = value.c_str();
    }

    void collectHeaders(const char* const* /*headerKeys*/, size_t /*count*/) {}

    void sendHeader(const String& name, const String& value, bool /*first*/ = false) {
        responseHeaders_[name.c_str()] = value.c_str();
    }

    String sentHeader(const String& name) const {
        auto it = responseHeaders_.find(name.c_str());
        if (it == responseHeaders_.end()) {
            return "";
        }
        return it->second.c_str();
    }

    void send(int code, const char* contentType, const String& body) {
        lastStatusCode = code;
        lastContentType = contentType ? contentType : "";
        lastBody = body;
        sendCount++;
    }

    void send(int code, const char* contentType, const char* body) {
        send(code, contentType, String(body ? body : ""));
    }

    int lastStatusCode = 0;
    String lastContentType;
    String lastBody;
    int sendCount = 0;

private:
    std::unordered_map<std::string, std::string> args_;
    std::unordered_map<std::string, std::string> requestHeaders_;
    std::unordered_map<std::string, std::string> responseHeaders_;
};
