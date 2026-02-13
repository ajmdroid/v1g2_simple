#include "gps_lockout_safety.h"

#include <cctype>
#include <string>

namespace {

bool isUnsignedIntString(const String& raw) {
    const char* cstr = raw.c_str();
    if (!cstr || *cstr == '\0') {
        return false;
    }
    for (const char* p = cstr; *p != '\0'; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
    }
    return true;
}

std::string trimAndLower(const String& raw) {
    std::string value = raw.c_str() ? raw.c_str() : "";
    size_t start = 0;
    size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    std::string out = value.substr(start, end - start);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

LockoutRuntimeMode gpsLockoutParseRuntimeModeArg(const String& raw,
                                                 LockoutRuntimeMode fallback) {
    const std::string token = trimAndLower(raw);
    if (token.empty()) {
        return fallback;
    }

    if (isUnsignedIntString(String(token.c_str()))) {
        int mode = String(token.c_str()).toInt();
        if (mode < 0) mode = 0;
        if (mode > 3) mode = 3;
        return static_cast<LockoutRuntimeMode>(mode);
    }

    if (token == "off") {
        return static_cast<LockoutRuntimeMode>(0);
    }
    if (token == "shadow") {
        return static_cast<LockoutRuntimeMode>(1);
    }
    if (token == "advisory") {
        return static_cast<LockoutRuntimeMode>(2);
    }
    if (token == "enforce") {
        return static_cast<LockoutRuntimeMode>(3);
    }

    return fallback;
}

GpsLockoutCoreGuardStatus gpsLockoutEvaluateCoreGuard(bool guardEnabled,
                                                      uint16_t maxQueueDrops,
                                                      uint16_t maxPerfDrops,
                                                      uint16_t maxEventBusDrops,
                                                      uint32_t queueDrops,
                                                      uint32_t perfDrops,
                                                      uint32_t eventBusDrops) {
    GpsLockoutCoreGuardStatus status;
    status.enabled = guardEnabled;
    if (!status.enabled) {
        return status;
    }

    if (queueDrops > maxQueueDrops) {
        status.tripped = true;
        status.reason = "queueDrops";
        return status;
    }

    if (perfDrops > maxPerfDrops) {
        status.tripped = true;
        status.reason = "perfDrop";
        return status;
    }

    if (eventBusDrops > maxEventBusDrops) {
        status.tripped = true;
        status.reason = "eventBusDrop";
        return status;
    }

    return status;
}
