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
};
