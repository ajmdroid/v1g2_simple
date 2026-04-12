/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "perf_sd_logger.h"
#include "modules/wifi/wifi_auto_timeout_module.h"
#include "modules/wifi/wifi_heap_guard_module.h"
#include "modules/wifi/wifi_stop_reason_module.h"
#include "esp_wifi.h"


// Global instance
WiFiManager wifiManager;

WiFiManager::WiFiManager() : server_(80), setupModeState_(SETUP_MODE_OFF), apInterfaceEnabled_(false), setupModeStartTime_(0) {
}

bool WiFiManager::isStopping() const {
    return setupModeState_ == SETUP_MODE_STOPPING;
}

bool WiFiManager::hasPendingLifecycleWork() const {
    return wifiStopPhase_ != WifiStopPhase::IDLE;
}

void WiFiManager::setBoundaryTransitionAdmission(const bool allow) {
    allowBoundaryTransitionWork_ = allow;
}

// Rate limiting: returns true if request is allowed, false if rate limited
bool WiFiManager::checkRateLimit() {
    const uint32_t now = millis();

    // Mark UI activity on every request
    markUiActivity();

    const SlidingWindowRateLimitDecision decision = rateLimiter_.evaluate(now);
    if (!decision.allowed) {
        const unsigned long roundedRetryAfter =
            static_cast<unsigned long>((decision.retryAfterMs + 999u) / 1000u);
        const unsigned long retryAfterSec = (roundedRetryAfter == 0) ? 1 : roundedRetryAfter;
        server_.sendHeader("Retry-After", String(retryAfterSec));
        server_.send(429, "application/json",
                    "{\"success\":false,\"message\":\"Too many requests\"}");
        return false;
    }

    return true;
}

// Web activity tracking for WiFi priority mode
void WiFiManager::markUiActivity() {
    lastUiActivityMs_ = millis();
}

bool WiFiManager::isUiActive(unsigned long timeoutMs) const {
    if (lastUiActivityMs_ == 0) return false;
    return (millis() - lastUiActivityMs_) < timeoutMs;
}
