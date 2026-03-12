#pragma once

#include <cstdint>

namespace obd {

// ── Scan parameters (web-UI-triggered only) ─────────────────────
static constexpr uint32_t SCAN_DURATION_MS = 5000;
static constexpr const char* DEVICE_NAME_PREFIX = "OBDLink";
static constexpr size_t DEVICE_NAME_PREFIX_LEN = 7;

// ── Direct-connect (saved address) ──────────────────────────────
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t RECONNECT_BACKOFF_MS = 60000;
static constexpr uint8_t MAX_DIRECT_CONNECT_FAILURES = 3;

// ── Boot dwell (V1 gets priority) ───────────────────────────────
static constexpr uint32_t POST_BOOT_DWELL_MS = 10000;

// ── Speed polling ───────────────────────────────────────────────
static constexpr uint32_t POLL_INTERVAL_MS = 500;
static constexpr uint32_t POLL_INTERVAL_SLOW_MS = 750;
static constexpr uint32_t POLL_TIMEOUT_MS = 400;
static constexpr uint32_t SPEED_MAX_AGE_MS = 3000;
static constexpr uint8_t MAX_CONSECUTIVE_ERRORS = 5;
static constexpr uint32_t ERROR_PAUSE_MS = 5000;
static constexpr uint8_t ERRORS_BEFORE_DISCONNECT = 10;

// ── RSSI ────────────────────────────────────────────────────────
static constexpr int8_t DEFAULT_MIN_RSSI = -80;

// ── RSSI cache refresh ─────────────────────────────────────────
static constexpr uint32_t RSSI_REFRESH_INTERVAL_MS = 2000;

// ── AT init sequence ────────────────────────────────────────────
static constexpr const char* AT_INIT_COMMANDS[] = {
    "ATZ\r",
    "ATE0\r",
    "ATL0\r",
    "ATS0\r",
    "ATH0\r",
    "ATSP0\r",
    "ATAT1\r",
};
static constexpr size_t AT_INIT_COMMAND_COUNT = sizeof(AT_INIT_COMMANDS) / sizeof(AT_INIT_COMMANDS[0]);
static constexpr uint32_t AT_INIT_RESPONSE_TIMEOUT_MS = 2000;

// ── Speed poll command ──────────────────────────────────────────
static constexpr const char* SPEED_POLL_CMD = "010D\r";

}  // namespace obd
