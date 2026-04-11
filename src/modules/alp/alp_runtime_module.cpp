/**
 * ALP Runtime Module — implementation.
 *
 * Rewritten April 2026 for the 4-byte frame protocol with 7-bit checksum.
 * All frames: byte0 byte1 byte2 checksum, where checksum = (b0+b1+b2) & 0x7F.
 *
 * Heavy logging throughout: this is early protocol integration and we need
 * visibility into every frame, state transition, and anomaly to validate
 * decoding against live captures. Logging will be dialed back once
 * field testing confirms reliability.
 */

#include "alp_runtime_module.h"
#include "alp_sd_logger.h"

extern AlpSdLogger alpSdLogger;

#include <cstring>

#ifndef UNIT_TEST
#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/gpio_filter.h>
#include <soc/gpio_struct.h>
#endif

// ── Global instance ──────────────────────────────────────────────────
// Follows the same pattern as ObdRuntimeModule in obd_runtime_module.cpp.
AlpRuntimeModule alpRuntimeModule;

// ── String helpers ───────────────────────────────────────────────────

const char* alpStateName(AlpState s) {
    switch (s) {
        case AlpState::OFF:           return "OFF";
        case AlpState::IDLE:          return "IDLE";
        case AlpState::LISTENING:     return "LISTENING";
        case AlpState::ALERT_ACTIVE:  return "ALERT_ACTIVE";
        case AlpState::NOISE_WINDOW:  return "NOISE_WINDOW";
        case AlpState::TEARDOWN:      return "TEARDOWN";
        default:                      return "UNKNOWN_STATE";
    }
}

const char* alpGunName(AlpGunType gun) {
    switch (gun) {
        case AlpGunType::UNKNOWN:           return "Unknown";
        case AlpGunType::PL3_PROLITE:       return "PL3 ProLite";
        case AlpGunType::DRAGONEYE_COMPACT: return "DragonEye Compact";
        case AlpGunType::LTI_TRUSPEED_LR:  return "LTI TruSpeed LR";
        case AlpGunType::LASER_ATLANTA_PL2: return "Laser Atlanta PL2";
        case AlpGunType::MARKSMAN_ULTRALYTE:return "Marksman Ultralyte";
        case AlpGunType::STALKER_LZ1:       return "Stalker LZ1";
        case AlpGunType::LASER_ALLY:        return "Laser Ally";
        case AlpGunType::ATLANTA_STEALTH:   return "Atlanta Stealth";
        default:                            return "INVALID_GUN";
    }
}

// 7-segment friendly abbreviations (~6 chars) for display frequency area.
// Only uses glyphs that render well on 7-seg: A b C d E F G H I J L n O P r S t U Y
const char* alpGunAbbrev(AlpGunType gun) {
    switch (gun) {
        case AlpGunType::PL3_PROLITE:       return "PL3";
        case AlpGunType::DRAGONEYE_COMPACT: return "drgEYE";
        case AlpGunType::LTI_TRUSPEED_LR:  return "truSPd";
        case AlpGunType::LASER_ATLANTA_PL2: return "PL2";
        case AlpGunType::MARKSMAN_ULTRALYTE:return "ULtrLt";
        case AlpGunType::STALKER_LZ1:       return "StLr";
        case AlpGunType::LASER_ALLY:        return "LALLY";
        case AlpGunType::ATLANTA_STEALTH:   return "StLtH";
        default:                            return "LASER";
    }
}

// ── Gun code lookup table ────────────────────────────────────────────
// Sources: ALP_PROTOCOL_REFERENCE.md (jam-mode codes) + live captures (observe-mode).
//
// Jam mode:     CX 00 YY — fingerprint is (byte0, byte2). Spec-derived codes.
// Observe mode: CX YY 00 — fingerprint is (byte0, byte1). Live-captured codes.
//
// In observe mode, byte0 matches the jam-mode gun family for the same gun.
// C8 00 04 is a generic "laser detected" trigger common to all guns in observe mode.

static constexpr AlpGunCode GUN_TABLE[] = {
    { 0xC8, 0xD5, AlpGunType::PL3_PROLITE },
    { 0xC8, 0xD6, AlpGunType::DRAGONEYE_COMPACT },
    { 0xC9, 0xF5, AlpGunType::LTI_TRUSPEED_LR },
    { 0xCB, 0xEB, AlpGunType::LASER_ATLANTA_PL2 },
    { 0xCD, 0xD6, AlpGunType::MARKSMAN_ULTRALYTE },
    { 0xCD, 0xEB, AlpGunType::STALKER_LZ1 },
    { 0xCD, 0xD7, AlpGunType::LASER_ALLY },
    { 0xCE, 0xEB, AlpGunType::ATLANTA_STEALTH },
};
static constexpr size_t GUN_TABLE_SIZE = sizeof(GUN_TABLE) / sizeof(GUN_TABLE[0]);

AlpGunType alpLookupGun(uint8_t byte0, uint8_t gunCode) {
    for (size_t i = 0; i < GUN_TABLE_SIZE; ++i) {
        if (GUN_TABLE[i].byte0 == byte0 && GUN_TABLE[i].gunCode == gunCode) {
            return GUN_TABLE[i].gun;
        }
    }
    return AlpGunType::UNKNOWN;
}

// ── Observe-mode gun lookup table ───────────────────────────────────
// Pattern: CX YY 00 where byte2=0x00, byte1!=0x00.
// Fingerprint is (byte0, byte1). Live-captured April 2026.
// byte0 matches the jam-mode gun family for the same physical gun.

static constexpr AlpGunCode OBS_GUN_TABLE[] = {
    { 0xC8, 0x0D, AlpGunType::PL3_PROLITE },          // live capture: PL3 ProLite
    { 0xC8, 0x11, AlpGunType::DRAGONEYE_COMPACT },    // live capture: DragonEye Compact
    { 0xC9, 0x0E, AlpGunType::LTI_TRUSPEED_LR },     // live capture: TruSpeed LR
    { 0xCB, 0x10, AlpGunType::LASER_ATLANTA_PL2 },    // live capture: Laser Atlanta PL2
    { 0xCD, 0x0C, AlpGunType::MARKSMAN_ULTRALYTE },   // live capture: Ultralyte
    { 0xCD, 0x0D, AlpGunType::STALKER_LZ1 },          // live capture: Stalker LZ1
    { 0xCD, 0x10, AlpGunType::LASER_ALLY },            // live capture: Laser Ally
    { 0xCE, 0x0C, AlpGunType::ATLANTA_STEALTH },      // live capture: Atlanta Stealth
};
static constexpr size_t OBS_GUN_TABLE_SIZE = sizeof(OBS_GUN_TABLE) / sizeof(OBS_GUN_TABLE[0]);

AlpGunType alpLookupGunObserve(uint8_t byte0, uint8_t byte1) {
    for (size_t i = 0; i < OBS_GUN_TABLE_SIZE; ++i) {
        if (OBS_GUN_TABLE[i].byte0 == byte0 && OBS_GUN_TABLE[i].gunCode == byte1) {
            return OBS_GUN_TABLE[i].gun;
        }
    }
    return AlpGunType::UNKNOWN;
}

// ── Logging helpers ──────────────────────────────────────────────────

#ifndef UNIT_TEST
static void logHex(const char* prefix, const uint8_t* data, size_t len) {
    Serial.printf("[ALP] %s (%u bytes): ", prefix, (unsigned)len);
    for (size_t i = 0; i < len; ++i) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
}
#else
#define logHex(prefix, data, len) ((void)0)
#endif

#ifndef UNIT_TEST
#define ALP_LOG(fmt, ...) Serial.printf("[ALP] " fmt "\n", ##__VA_ARGS__)
// ALP_TRACE: high-frequency per-frame logging. Off by default.
// Enable via build flag -DALP_TRACE_ENABLED for serial capture sessions.
#ifdef ALP_TRACE_ENABLED
#define ALP_TRACE(fmt, ...) Serial.printf("[ALP] " fmt "\n", ##__VA_ARGS__)
#else
#define ALP_TRACE(fmt, ...) ((void)0)
#endif
#else
#define ALP_LOG(fmt, ...) ((void)0)
#define ALP_TRACE(fmt, ...) ((void)0)
#endif

// ── begin() ──────────────────────────────────────────────────────────

void AlpRuntimeModule::begin(bool enabled) {
    enabled_ = enabled;
    begun_ = true;

    if (!enabled) {
        state_ = AlpState::OFF;
        ALP_LOG("begin: disabled");
        return;
    }

#ifndef UNIT_TEST
    // Configure enable pin (active HIGH, matches former GPS usage)
    pinMode(ALP_EN_PIN, OUTPUT);
    digitalWrite(ALP_EN_PIN, HIGH);
    ALP_LOG("begin: EN pin %d HIGH", ALP_EN_PIN);

    // Configure UART2 at 19200 8N1
    Serial2.setRxBufferSize(UART_RX_BUFFER_SIZE);
    Serial2.begin(ALP_BAUD, SERIAL_8N1, ALP_RX_PIN, ALP_TX_PIN);
    ALP_LOG("begin: UART2 open baud=%lu RX=%d TX=%d bufSize=%u",
            (unsigned long)ALP_BAUD, ALP_RX_PIN, ALP_TX_PIN,
            (unsigned)UART_RX_BUFFER_SIZE);

    // GPIO glitch filter on RX pin — rejects sub-10µs I2S crosstalk
    // during active jamming while passing valid 52.1µs UART bits.
    // Requires proper ground reference between ALP and ESP32.
    gpio_glitch_filter_handle_t filterHandle = nullptr;
    gpio_pin_glitch_filter_config_t filterConfig = {};
    filterConfig.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT;
    filterConfig.gpio_num = static_cast<gpio_num_t>(ALP_RX_PIN);
    esp_err_t err = gpio_new_pin_glitch_filter(&filterConfig, &filterHandle);
    if (err == ESP_OK && filterHandle) {
        gpio_glitch_filter_enable(filterHandle);
        ALP_LOG("begin: GPIO glitch filter enabled on pin %d", ALP_RX_PIN);
    } else {
        ALP_LOG("begin: WARNING — GPIO glitch filter failed err=%d", err);
    }
#endif

    state_ = AlpState::IDLE;
    ALP_LOG("begin: enabled -> IDLE");
}

// ── process() — main loop entry ──────────────────────────────────────

void AlpRuntimeModule::process(uint32_t nowMs) {
    if (!begun_ || !enabled_) return;

    // Drain UART into ring buffer
    drainUart(nowMs);

    // Timeout checks (state-dependent)
    switch (state_) {
        case AlpState::LISTENING:
            handleHeartbeatTimeout(nowMs);
            break;
        case AlpState::ALERT_ACTIVE:
            handleAlertActiveTimeout(nowMs);
            break;
        case AlpState::NOISE_WINDOW:
            handleNoiseWindowTimeout(nowMs);
            break;
        case AlpState::TEARDOWN:
            handleTeardownTimeout(nowMs);
            break;
        default:
            break;
    }

    // Parse whatever is in the ring buffer
    parseRingBuffer(nowMs);
}

// ── snapshot() ───────────────────────────────────────────────────────

AlpStatus AlpRuntimeModule::snapshot() const {
    AlpStatus s;
    s.state = state_;
    s.lastGun = lastGun_;
    s.lastGunTimestampMs = lastGunTimestampMs_;
    s.lastHeartbeatMs = lastHeartbeatMs_;
    s.statusBurstCount = statusBurstCount_;
    s.heartbeatCount = heartbeatCount_;
    s.frameErrors = frameErrors_;
    s.noiseWindowCount = noiseWindowCount_;
    s.lastHbByte1 = lastHbByte1_;
    s.uartActive = uartHasReceivedData_;
    return s;
}

// ── State transitions ────────────────────────────────────────────────

void AlpRuntimeModule::transitionTo(AlpState newState, uint32_t nowMs) {
    ALP_LOG("state: %s -> %s at %lu ms",
            alpStateName(state_), alpStateName(newState),
            (unsigned long)nowMs);
    alpSdLogger.logStateTransition(nowMs, state_, newState);
    state_ = newState;
}

// ── UART drain ───────────────────────────────────────────────────────

void AlpRuntimeModule::drainUart(uint32_t nowMs) {
#ifndef UNIT_TEST
    const int available = Serial2.available();
    if (available <= 0) return;

    // First data ever — log it
    if (!uartHasReceivedData_) {
        uartHasReceivedData_ = true;
        ALP_LOG("UART first data at %lu ms (%d bytes available)",
                (unsigned long)nowMs, available);
    }

    // Read into ring buffer, up to remaining capacity
    const size_t space = RING_CAPACITY - ringLen_;
    if (space == 0) {
        // Ring full — discard oldest bytes to make room
        ALP_LOG("WARNING: ring buffer full, discarding %u bytes", (unsigned)(RING_CAPACITY / 2));
        const size_t keep = RING_CAPACITY / 2;
        memmove(ringBuf_, ringBuf_ + (RING_CAPACITY - keep), keep);
        ringLen_ = keep;
        frameErrors_++;
    }

    const size_t toRead = (space < (size_t)available) ? space : (size_t)available;
    const size_t bytesRead = Serial2.readBytes(ringBuf_ + ringLen_, toRead);
    ringLen_ += bytesRead;
#endif
}

// ── Ring buffer parsing ──────────────────────────────────────────────
// All frames are 4 bytes with 7-bit checksum. On valid checksum, dispatch
// by byte0. On invalid checksum, advance 1 byte to resync. Consecutive
// checksum failures from ALERT_ACTIVE trigger NOISE_WINDOW transition.

void AlpRuntimeModule::parseRingBuffer(uint32_t nowMs) {
    int maxIterations = 32;

    while (ringLen_ >= FRAME_LEN && maxIterations-- > 0) {
        // Try to parse a valid 4-byte frame at current position
        if (tryParseFrame(nowMs)) continue;

        // Checksum failed — noise or misalignment
        consecutiveBadChecksums_++;
        frameErrors_++;

        // Speaker noise causes UART flood in BOTH jam and observe mode.
        // Enter NOISE_WINDOW from ALERT_ACTIVE or LISTENING (if we've
        // seen alert-mode heartbeats) after enough consecutive failures.
        if (consecutiveBadChecksums_ >= NOISE_CHECKSUM_THRESHOLD &&
            state_ != AlpState::NOISE_WINDOW) {
            bool enterNoise = false;
            if (state_ == AlpState::ALERT_ACTIVE) {
                enterNoise = true;
            } else if (state_ == AlpState::LISTENING && alertDetectedViaHb_) {
                // Heartbeat byte1=01 told us alert is active, then noise hit
                // before we saw a 98 trigger. Transition through ALERT_ACTIVE.
                ALP_LOG("ALERT via heartbeat byte1=01 + noise — entering ALERT_ACTIVE");
                transitionTo(AlpState::ALERT_ACTIVE, nowMs);
                statusBurstCount_++;
                enterNoise = true;
            }
            if (enterNoise) {
                ALP_LOG("NOISE: %lu consecutive bad checksums — entering NOISE_WINDOW",
                        (unsigned long)consecutiveBadChecksums_);
                alpSdLogger.logEvent(nowMs, "NOISE_ENTER", state_, consecutiveBadChecksums_);
                transitionTo(AlpState::NOISE_WINDOW, nowMs);
                noiseWindowEntryMs_ = nowMs;
                noiseWindowCount_++;
            }
        }

        // During NOISE_WINDOW, drain the buffer efficiently (no per-byte log)
        if (state_ == AlpState::NOISE_WINDOW) {
            if (ringLen_ > FRAME_LEN) {
                size_t discard = ringLen_ - (FRAME_LEN - 1);
                consumeBytes(discard);
            } else {
                consumeBytes(1);
            }
            continue;
        }

        // Not in noise window — throttled RESYNC logging
        if (consecutiveBadChecksums_ <= 3 ||
            (consecutiveBadChecksums_ % RESYNC_LOG_INTERVAL) == 0) {
            ALP_TRACE("RESYNC: bad checksum at 0x%02X %02X %02X %02X (err#%lu)",
                    ringBuf_[0], ringBuf_[1], ringBuf_[2], ringBuf_[3],
                    (unsigned long)consecutiveBadChecksums_);
        }
        consumeBytes(1);
    }
}

// ── Frame parser (checksum-validated dispatch) ───────────────────────

bool AlpRuntimeModule::tryParseFrame(uint32_t nowMs) {
    if (ringLen_ < FRAME_LEN) return false;

    const uint8_t b0 = ringBuf_[0];
    const uint8_t b1 = ringBuf_[1];
    const uint8_t b2 = ringBuf_[2];
    const uint8_t cs = ringBuf_[3];

    // Validate checksum — the core integrity check
    if (!alpValidateChecksum(b0, b1, b2, cs)) return false;

    // Valid frame — reset bad checksum counter
    consecutiveBadChecksums_ = 0;

    // If we were in NOISE_WINDOW, first valid frame = teardown
    if (state_ == AlpState::NOISE_WINDOW) {
        ALP_LOG("NOISE_WINDOW ended — first valid frame %02X %02X %02X %02X after %lu ms",
                b0, b1, b2, cs, (unsigned long)(nowMs - noiseWindowEntryMs_));
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }

    // Dispatch by byte0 range
    if (b0 == ALERT_BYTE0) {
        handleAlertFrame(b1, b2, nowMs);
    } else if (b0 == HEARTBEAT_SINGLE_0 || b0 == HEARTBEAT_PAIRED_0 ||
               b0 == HEARTBEAT_TRIPLE_0 || b0 == SETUP_BYTE0_A8 ||
               b0 == SETUP_BYTE0_F0) {
        handleHeartbeatFrame(b0, b1, b2, nowMs);
    } else if (b0 >= 0xC8 && b0 <= 0xCE) {
        handleGunCandidate(b0, b1, b2, nowMs);
    } else if (b0 >= 0xD0 && b0 <= 0xD3) {
        handleRegisterFrame(b0, b1, b2, nowMs);
    } else if (b0 == DISCOVERY_BYTE0) {
        handleDiscoveryFrame(b1, b2, nowMs);
    } else {
        // Valid checksum but unrecognized byte0 — treat as sign of life
        ALP_TRACE("UNKNOWN_FRAME: %02X %02X %02X %02X (valid checksum)", b0, b1, b2, cs);
        lastHeartbeatMs_ = nowMs;
        lastFrameMs_ = nowMs;
    }

    consumeBytes(FRAME_LEN);
    return true;
}

// ── Frame handlers ──────────────────────────────────────────────────

void AlpRuntimeModule::handleAlertFrame(uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    if (b1 == ALERT_BYTE1 && b2 == ALERT_BYTE2) {
        // Jam-mode alert trigger: 98 00 E3 — laser event onset with jamming
        statusBurstCount_++;
        lastAlertTriggerMs_ = nowMs;
        ALP_LOG("ALERT_TRIGGER: 98 00 E3 (jam mode) — burst #%lu",
                (unsigned long)statusBurstCount_);
        alpSdLogger.logFrame(nowMs, "ALERT_JAM", ALERT_BYTE0, b1, b2,
                             alpChecksum(ALERT_BYTE0, b1, b2), state_);

        if (state_ != AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::ALERT_ACTIVE, nowMs);
        }
    } else if (b1 == 0x02 && b2 == 0x00) {
        // Observe-mode alert trigger: 98 02 00 — detection only, no jamming
        statusBurstCount_++;
        lastAlertTriggerMs_ = nowMs;
        ALP_LOG("ALERT_TRIGGER: 98 02 00 (observe mode) — burst #%lu",
                (unsigned long)statusBurstCount_);
        alpSdLogger.logFrame(nowMs, "ALERT_OBS", ALERT_BYTE0, b1, b2,
                             alpChecksum(ALERT_BYTE0, b1, b2), state_);

        if (state_ != AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::ALERT_ACTIVE, nowMs);
        }
    } else {
        // Other 98 XX YY frames (status/config)
        ALP_TRACE("STATUS_FRAME: 98 %02X %02X  state=%s",
                b1, b2, alpStateName(state_));

        if (state_ == AlpState::IDLE) {
            transitionTo(AlpState::LISTENING, nowMs);
        }
        // TEARDOWN: don't reset teardownEntryMs_ — let the timeout expire naturally.
    }
}

void AlpRuntimeModule::handleHeartbeatFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;
    heartbeatCount_++;

    ALP_TRACE("HEARTBEAT: %02X %02X %02X  state=%s  hb#=%lu",
              b0, b1, b2, alpStateName(state_),
              (unsigned long)heartbeatCount_);

    // SD trace: log every heartbeat frame to capture the full cycling
    // pattern (byte1: 02→03→04→02...) that may encode scan/armed state.
    alpSdLogger.logHeartbeat(nowMs, b0, b1, b2, state_);

    // ── Heartbeat byte1 alert detection (B0 frames only) ────────────
    // B0 heartbeats carry alert status in byte1:
    //   byte1=01 → laser detected (alert active)
    //   byte1=02/03/04 → idle (cycling through modes)
    // This is the universal alert indicator — works in jam AND observe mode.
    if (b0 == HEARTBEAT_SINGLE_0) {
        uint8_t prevByte1 = lastHbByte1_;
        lastHbByte1_ = b1;

        // During TEARDOWN, byte1 toggles rapidly between 01/00 as the CPU
        // does post-alert housekeeping.  These aren't real alert events —
        // ignore them to avoid log spam and spurious state transitions.
        if (state_ != AlpState::TEARDOWN) {
            if (b1 == HB_BYTE1_ALERT && prevByte1 != HB_BYTE1_ALERT) {
                // Transition to alert — heartbeat just flipped to 01
                alertDetectedViaHb_ = true;
                ALP_LOG("HB ALERT: byte1 %02X -> 01 — laser detected via heartbeat",
                        prevByte1);
                alpSdLogger.logHeartbeatByte1(nowMs, prevByte1, b1, state_);

                if (state_ == AlpState::LISTENING) {
                    transitionTo(AlpState::ALERT_ACTIVE, nowMs);
                    statusBurstCount_++;
                }
            } else if (b1 != HB_BYTE1_ALERT && prevByte1 == HB_BYTE1_ALERT) {
                // Transition back to idle — alert resolved
                alertDetectedViaHb_ = false;
                ALP_LOG("HB IDLE: byte1 01 -> %02X — alert resolved via heartbeat", b1);
                alpSdLogger.logHeartbeatByte1(nowMs, prevByte1, b1, state_);

                if (state_ == AlpState::ALERT_ACTIVE) {
                    transitionTo(AlpState::TEARDOWN, nowMs);
                    teardownEntryMs_ = nowMs;
                }
            }
        }
    }

    // State transitions (generic — applies to all heartbeat types)
    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
    // TEARDOWN: don't reset teardownEntryMs_ — let the timeout expire naturally.
}

void AlpRuntimeModule::handleGunCandidate(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;
    heartbeatCount_++;

    // Gun fingerprint — two patterns:
    //   Jam mode:     CX 00 YY — byte1=0x00, fingerprint (byte0, byte2)
    //   Observe mode: CX YY 00 — byte2=0x00, byte1!=0x00, fingerprint (byte0, byte1)
    AlpGunType gun = AlpGunType::UNKNOWN;
    if (b1 == 0x00) {
        gun = alpLookupGun(b0, b2);
        if (gun != AlpGunType::UNKNOWN) {
            ALP_LOG("GUN IDENTIFIED (jam): byte0=0x%02X gunCode=0x%02X -> %s",
                    b0, b2, alpGunName(gun));
            alpSdLogger.logGunIdentified(nowMs, gun, b0, b2, false, state_);
        }
    }
    if (gun == AlpGunType::UNKNOWN && b2 == 0x00 && b1 != 0x00) {
        gun = alpLookupGunObserve(b0, b1);
        if (gun != AlpGunType::UNKNOWN) {
            ALP_LOG("GUN IDENTIFIED (observe): byte0=0x%02X byte1=0x%02X -> %s",
                    b0, b1, alpGunName(gun));
            alpSdLogger.logGunIdentified(nowMs, gun, b0, b1, true, state_);
        }
    }
    if (gun != AlpGunType::UNKNOWN) {
        lastGun_ = gun;
        lastGunTimestampMs_ = nowMs;
    }

    ALP_TRACE("C_FRAME: %02X %02X %02X  state=%s", b0, b1, b2, alpStateName(state_));

    // State transitions
    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
    // TEARDOWN: don't reset teardownEntryMs_ — let the timeout expire naturally.
}

void AlpRuntimeModule::handleRegisterFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;
    heartbeatCount_++;

    ALP_TRACE("REGISTER_WRITE: %02X %02X %02X  state=%s", b0, b1, b2, alpStateName(state_));

    // FD terminator at byte2 signals return-to-idle
    if (b2 == 0xFD) {
        ALP_LOG("FD terminator in register write — entering teardown");
        if (state_ == AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::TEARDOWN, nowMs);
            teardownEntryMs_ = nowMs;
        }
    }

    // State transitions
    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
    // TEARDOWN: don't reset teardownEntryMs_ — let the timeout expire naturally.
}

void AlpRuntimeModule::handleDiscoveryFrame(uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    ALP_TRACE("DISCOVERY: 91 %02X %02X (CPU polling for control set)", b1, b2);

    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

// ── Ring buffer management ───────────────────────────────────────────

void AlpRuntimeModule::consumeBytes(size_t count) {
    if (count >= ringLen_) {
        ringLen_ = 0;
        return;
    }
    memmove(ringBuf_, ringBuf_ + count, ringLen_ - count);
    ringLen_ -= count;
}

// ── Timeout handlers ─────────────────────────────────────────────────

void AlpRuntimeModule::handleHeartbeatTimeout(uint32_t nowMs) {
    if (lastHeartbeatMs_ == 0) return;
    if (nowMs - lastHeartbeatMs_ > HEARTBEAT_TIMEOUT_MS) {
        ALP_LOG("HEARTBEAT TIMEOUT: no frame for %lu ms — ALP CPU silent",
                (unsigned long)(nowMs - lastHeartbeatMs_));
        transitionTo(AlpState::IDLE, nowMs);
        lastHeartbeatMs_ = 0;
    }
}

void AlpRuntimeModule::handleNoiseWindowTimeout(uint32_t nowMs) {
    if (nowMs - noiseWindowEntryMs_ > NOISE_WINDOW_MAX_MS) {
        ALP_LOG("NOISE_WINDOW TIMEOUT: no clean frame after %lu ms — forced teardown",
                (unsigned long)(nowMs - noiseWindowEntryMs_));
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }
}

void AlpRuntimeModule::handleTeardownTimeout(uint32_t nowMs) {
    if (nowMs - teardownEntryMs_ > TEARDOWN_TIMEOUT_MS) {
        ALP_LOG("TEARDOWN complete: returning to LISTENING after %lu ms",
                (unsigned long)(nowMs - teardownEntryMs_));
        alertDetectedViaHb_ = false;
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleAlertActiveTimeout(uint32_t nowMs) {
    if (lastAlertTriggerMs_ != 0 &&
        nowMs - lastAlertTriggerMs_ > ALERT_ACTIVE_TIMEOUT_MS) {
        ALP_LOG("ALERT_ACTIVE timeout: no 98 trigger rearm in %lu ms — teardown",
                (unsigned long)(nowMs - lastAlertTriggerMs_));
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }
}

// ── Test instrumentation ─────────────────────────────────────────────

#ifdef UNIT_TEST
void AlpRuntimeModule::testInjectBytes(const uint8_t* data, size_t len) {
    const size_t space = RING_CAPACITY - ringLen_;
    const size_t toWrite = (len < space) ? len : space;
    memcpy(ringBuf_ + ringLen_, data, toWrite);
    ringLen_ += toWrite;
    if (!uartHasReceivedData_ && toWrite > 0) {
        uartHasReceivedData_ = true;
    }
}
#endif
