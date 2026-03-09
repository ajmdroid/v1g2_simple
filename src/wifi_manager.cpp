/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "perf_sd_logger.h"
#include "time_service.h"
#include "modules/wifi/wifi_auto_timeout_module.h"
#include "modules/wifi/wifi_heap_guard_module.h"
#include "modules/wifi/wifi_stop_reason_module.h"
#include "esp_wifi.h"


// Global instance
WiFiManager wifiManager;

WiFiManager::WiFiManager() : server(80), setupModeState(SETUP_MODE_OFF), apInterfaceEnabled(false), setupModeStartTime(0), rateLimitWindowStart(0), rateLimitRequestCount(0) {
}

// Rate limiting: returns true if request is allowed, false if rate limited
bool WiFiManager::checkRateLimit() {
    unsigned long now = millis();
    
    // Mark UI activity on every request
    markUiActivity();
    
    // Reset window if expired
    if (now - rateLimitWindowStart > RATE_LIMIT_WINDOW_MS) {
        rateLimitWindowStart = now;
        rateLimitRequestCount = 0;
    }
    
    rateLimitRequestCount++;
    
    if (rateLimitRequestCount > RATE_LIMIT_MAX_REQUESTS) {
        unsigned long retryAfterSec = 1;
        if (now >= rateLimitWindowStart && (now - rateLimitWindowStart) < RATE_LIMIT_WINDOW_MS) {
            retryAfterSec = ((RATE_LIMIT_WINDOW_MS - (now - rateLimitWindowStart)) + 999) / 1000;
        }
        server.sendHeader("Retry-After", String(retryAfterSec));
        server.send(429, "application/json",
                    "{\"success\":false,\"message\":\"Too many requests\"}");
        return false;
    }
    
    return true;
}

// Web activity tracking for WiFi priority mode
void WiFiManager::markUiActivity() {
    lastUiActivityMs = millis();
}

bool WiFiManager::isUiActive(unsigned long timeoutMs) const {
    if (lastUiActivityMs == 0) return false;
    return (millis() - lastUiActivityMs) < timeoutMs;
}
