/**
 * ALP SD Logger — CSV event logger for ALP state machine.
 *
 * Logs state transitions, heartbeat byte1 changes, gun identifications,
 * and alert/noise/teardown events to a CSV file on SD card. Designed
 * for field capture of ALP serial patterns during drives to help
 * distinguish scan-vs-armed states and refine badge color mapping.
 *
 * Design:
 *   - Direct file append from main loop (no FreeRTOS queue — events
 *     are infrequent, maybe a few per minute at most).
 *   - Uses SDTryLock (non-blocking) since process() runs on Core 1.
 *   - CSV file per boot session: /alp/alp_<bootId>.csv
 *   - Controlled by alpSdLogEnabled setting (dev page toggle).
 *   - All methods are no-ops when disabled or SD unavailable.
 */

#pragma once

#include <cstdint>

// Forward declarations — no heavy includes in the header
enum class AlpState : uint8_t;
enum class AlpGunType : uint8_t;

class AlpSdLogger {
public:
    /**
     * Initialize the logger.
     * @param enabled   true if alpSdLogEnabled setting is on
     * @param sdReady   true if StorageManager has SD mounted
     */
    void begin(bool enabled, bool sdReady);

    /**
     * Set boot ID for filename generation. Call after begin().
     */
    void setBootId(uint32_t id);

    /**
     * Log a state transition.
     */
    void logStateTransition(uint32_t nowMs, AlpState from, AlpState to);

    /**
     * Log a heartbeat byte1 change (alert detection signal).
     */
    void logHeartbeatByte1(uint32_t nowMs, uint8_t prevByte1, uint8_t newByte1, AlpState currentState);

    /**
     * Log a gun identification event.
     */
    void logGunIdentified(uint32_t nowMs, AlpGunType gun, uint8_t byte0, uint8_t byte1or2,
                          bool isObserveMode, AlpState currentState);

    /**
     * Log a raw frame (for high-value frames like alert triggers, gun candidates).
     */
    void logFrame(uint32_t nowMs, const char* frameType,
                  uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs,
                  AlpState currentState);

    /**
     * Log every B0 heartbeat frame (byte0, byte1, byte2) for trace-level
     * analysis of LISTENING sub-state cycling. Called on every B0 frame
     * regardless of whether byte1 changed — the cycling pattern within
     * 02/03/04 may encode scan-vs-armed state visible on the control pad LED.
     *
     * Rate-limited: writes at most one row per HEARTBEAT_LOG_INTERVAL_MS
     * to avoid flooding the SD card (heartbeats arrive every ~800ms).
     */
    void logHeartbeat(uint32_t nowMs, uint8_t b0, uint8_t b1, uint8_t b2,
                      AlpState currentState);

    /**
     * Log a noise window entry/exit or teardown event.
     */
    void logEvent(uint32_t nowMs, const char* event, AlpState currentState,
                  uint32_t extraValue = 0);

    /** Is logging active? */
    bool isEnabled() const { return enabled_ && sdReady_; }

    /** Get the CSV file path. */
    const char* csvPath() const { return csvPathBuf_; }

    /** Update enabled state at runtime (setting changed via web UI). */
    void setEnabled(bool enabled);

private:
    bool appendLine(const char* line);
    bool ensureDirectory();
    bool ensureHeader();

    // Log every heartbeat but rate-limit to avoid SD flood.
    // Heartbeats arrive ~800ms apart; log every one to capture the
    // full cycling pattern (02→03→04→02...) that may encode scan/armed.
    static constexpr uint32_t HEARTBEAT_LOG_INTERVAL_MS = 0;  // 0 = every heartbeat

    bool enabled_ = false;
    bool sdReady_ = false;
    bool dirReady_ = false;
    bool headerWritten_ = false;
    uint32_t bootId_ = 0;
    uint32_t linesWritten_ = 0;
    uint32_t dropCount_ = 0;
    uint32_t lastHeartbeatLogMs_ = 0;
    char csvPathBuf_[48] = {0};
};
