/**
 * Push Executor - Transactional Auto-Push Pipeline
 * 
 * Implements reliable, verified settings push to V1 with:
 * - Ordered command execution (profile → display → mode → volume)
 * - Verification via readback/status packets
 * - Retry logic with bounded attempts
 * - Backpressure awareness (pauses if BLE RX queue grows)
 * - Per-device mapping resolution
 * - Instrumentation for debugging
 */

#ifndef PUSH_EXECUTOR_H
#define PUSH_EXECUTOR_H

#include <Arduino.h>
#include "v1_profiles.h"
#include "settings.h"

// Push command types in execution order
enum PushCommandType {
    PUSH_CMD_NONE = 0,
    PUSH_CMD_USER_BYTES,    // Profile settings (6 bytes)
    PUSH_CMD_DISPLAY,       // Display on/off
    PUSH_CMD_MODE,          // Operating mode (All Bogeys, Logic, etc.)
    PUSH_CMD_VOLUME,        // Volume levels
};

// Push command status
enum PushCommandStatus {
    CMD_STATUS_PENDING = 0,
    CMD_STATUS_SENT,
    CMD_STATUS_VERIFIED,
    CMD_STATUS_FAILED,
    CMD_STATUS_SKIPPED,     // Intentionally skipped (not configured)
};

// Individual command in the push plan
struct PushCommand {
    PushCommandType type = PUSH_CMD_NONE;
    PushCommandStatus status = CMD_STATUS_PENDING;
    uint8_t retryCount = 0;
    uint32_t sentAtMs = 0;
    uint32_t verifiedAtMs = 0;
    
    // Command-specific data
    union {
        struct { uint8_t bytes[6]; } userBytes;
        struct { bool on; } display;
        struct { uint8_t mode; } modeCmd;
        struct { uint8_t main; uint8_t muted; } volume;
    } data;
};

// Overall push state
enum PushState {
    PUSH_STATE_IDLE = 0,
    PUSH_STATE_RESOLVING,       // Looking up per-device mapping
    PUSH_STATE_PLANNING,        // Building command plan
    PUSH_STATE_EXECUTING,       // Running commands
    PUSH_STATE_VERIFYING,       // Waiting for verification
    PUSH_STATE_SUCCESS,         // All verified
    PUSH_STATE_FAILED,          // Failed after retries
};

// Push result for API
enum PushResult {
    PUSH_RESULT_NONE = 0,
    PUSH_RESULT_IN_PROGRESS,
    PUSH_RESULT_SUCCESS,
    PUSH_RESULT_PARTIAL,        // Some commands succeeded
    PUSH_RESULT_FAILED,
    PUSH_RESULT_TIMEOUT,
    PUSH_RESULT_DISCONNECTED,
};

// Push metrics for instrumentation
struct PushMetrics {
    uint32_t totalPushes = 0;
    uint32_t successCount = 0;
    uint32_t failCount = 0;
    uint32_t partialCount = 0;
    uint32_t timeoutCount = 0;
    uint32_t disconnectCount = 0;
    uint32_t totalRetries = 0;
    uint32_t cmdsSent = 0;
    uint32_t cmdsVerified = 0;
    uint32_t cmdsFailed = 0;
    uint32_t backpressurePauses = 0;
    uint32_t avgPushDurationMs = 0;
    uint32_t lastPushDurationMs = 0;
    String lastFailReason;
    
    void reset() {
        totalPushes = 0;
        successCount = 0;
        failCount = 0;
        partialCount = 0;
        timeoutCount = 0;
        disconnectCount = 0;
        totalRetries = 0;
        cmdsSent = 0;
        cmdsVerified = 0;
        cmdsFailed = 0;
        backpressurePauses = 0;
        avgPushDurationMs = 0;
        lastPushDurationMs = 0;
        lastFailReason = "";
    }
};

// Push plan - complete transaction
struct PushPlan {
    static constexpr int MAX_COMMANDS = 4;  // user_bytes, display, mode, volume
    
    PushState state = PUSH_STATE_IDLE;
    PushResult result = PUSH_RESULT_NONE;
    String targetV1Address;
    int resolvedSlot = -1;
    
    PushCommand commands[MAX_COMMANDS];
    int commandCount = 0;
    int currentCommandIndex = 0;
    
    uint32_t startedAtMs = 0;
    uint32_t lastActivityMs = 0;
    uint8_t totalRetries = 0;
    String failReason;
    
    // Verification data received from V1
    bool userBytesReceived = false;
    uint8_t receivedUserBytes[6] = {0};
    bool modeReceived = false;
    uint8_t receivedMode = 0;
    
    void reset() {
        state = PUSH_STATE_IDLE;
        result = PUSH_RESULT_NONE;
        targetV1Address = "";
        resolvedSlot = -1;
        commandCount = 0;
        currentCommandIndex = 0;
        startedAtMs = 0;
        lastActivityMs = 0;
        totalRetries = 0;
        failReason = "";
        userBytesReceived = false;
        memset(receivedUserBytes, 0, 6);
        modeReceived = false;
        receivedMode = 0;
        for (int i = 0; i < MAX_COMMANDS; i++) {
            commands[i] = PushCommand();
        }
    }
};

// Forward declarations
class V1BLEClient;
class V1ProfileManager;
class SettingsManager;

class PushExecutor {
public:
    PushExecutor();
    
    // Initialize with dependencies
    void begin(V1BLEClient* ble, V1ProfileManager* profiles, SettingsManager* settings);
    
    // Start a new push transaction for connected V1
    // Returns false if push already in progress
    bool startPush(const String& v1Address, int slotOverride = -1);
    
    // Cancel current push
    void cancelPush(const String& reason = "cancelled");
    
    // Process push state machine - call from main loop
    // latencyMs: current BLE→DRAW latency for backpressure
    // Returns true if push made progress
    bool process(uint32_t latencyMs = 0);
    
    // Check if push is active
    bool isActive() const { return plan.state != PUSH_STATE_IDLE; }
    
    // Get current state/result
    PushState getState() const { return plan.state; }
    PushResult getResult() const { return plan.result; }
    const String& getFailReason() const { return plan.failReason; }
    
    // Called when V1 sends user bytes response (for verification)
    void onUserBytesReceived(const uint8_t* bytes);
    
    // Called when V1 sends mode in status packet (for verification)
    void onModeReceived(uint8_t mode);
    
    // Get metrics
    const PushMetrics& getMetrics() const { return metrics; }
    void resetMetrics() { metrics.reset(); }
    
    // Get current plan (read-only) for status API
    const PushPlan& getPlan() const { return plan; }
    
    // Configuration
    static constexpr uint32_t CMD_TIMEOUT_MS = 2000;        // Per-command timeout
    static constexpr uint32_t VERIFY_TIMEOUT_MS = 3000;     // Verification timeout
    static constexpr uint32_t TOTAL_TIMEOUT_MS = 15000;     // Total push timeout
    static constexpr uint8_t MAX_RETRIES = 2;               // Per-command retries
    static constexpr uint32_t INTER_CMD_DELAY_MS = 100;     // Delay between commands
    static constexpr uint32_t BACKPRESSURE_THRESHOLD_MS = 500; // Pause if latency exceeds this

private:
    V1BLEClient* bleClient = nullptr;
    V1ProfileManager* profileManager = nullptr;
    SettingsManager* settingsManager = nullptr;
    
    PushPlan plan;
    PushMetrics metrics;
    
    // Resolve per-device slot mapping
    int resolveDeviceSlot(const String& v1Address);
    
    // Build command plan from slot settings
    bool buildPlan(int slotIndex);
    
    // Execute current command
    bool executeCurrentCommand();
    
    // Check verification status
    bool checkVerification();
    
    // Handle command completion
    void completeCurrentCommand(bool success);
    
    // Finish push with result
    void finishPush(PushResult result, const String& reason = "");
};

// Global instance
extern PushExecutor pushExecutor;

#endif // PUSH_EXECUTOR_H
