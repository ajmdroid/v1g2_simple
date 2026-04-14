/**
 * ALP Runtime Module — Active Laser Protection serial listener.
 *
 * Owns UART2 RX on GPIO 1 (ALP CPU TX, RJ-45 pin 2).
 * Receives ALP HiFi serial data at 19200 8N1, parses alert frames
 * for gun identification, and tracks connection/jamming lifecycle.
 * GPIO 2 and 3 reserved for future control pad RX/TX.
 *
 * Design notes:
 *   - Receive-only: the ESP32 only listens to CPU TX (pin 2 on RJ-45).
 *   - ALL frames are 4 bytes: byte0 byte1 byte2 checksum.
 *     checksum = (byte0 + byte1 + byte2) & 0x7F.
 *     Verified against every clean frame from live captures April 2026.
 *   - Alert detection uses two signals:
 *     1. Jam-mode trigger: 98 00 E3 7B (logic analyzer captures, jam active)
 *     2. Heartbeat byte1 transition: B0 01 XX = alert, B0 02/03/04 XX = idle
 *     The heartbeat transition is the universal indicator — works in both
 *     jam mode and observe mode (no return pulses).
 *   - Gun identification comes from CX 00 YY frames in the alert burst.
 *     Jam mode: full gun codes (CD 00 D6 = Ultralyte). Observe mode: may
 *     differ (C8 00 04 seen for Ultralyte). Gun table covers jam-mode codes.
 *   - During laser events, UART is flooded with noise from detection
 *     circuitry crosstalk (observe mode) or I2S speaker crosstalk (jam mode).
 *     Checksum validation rejects noise; GPIO glitch filter adds HW rejection.
 *   - This module does NOT yet override V1 laser alerts. The V1 remains
 *     the authoritative laser source until field validation is complete.
 *
 * Wiring: begin() with enable flag. process() from main loop.
 * No std::function. No globals. Dependencies injected.
 */

#pragma once

#include <cstdint>
#include <cstddef>

class AlpSdLogger;

// ── ALP connection / protocol states ─────────────────────────────────

enum class AlpState : uint8_t {
    OFF = 0,            // Module off (alpEnabled == false)
    IDLE,               // UART open, waiting for first valid frame
    LISTENING,          // Receiving heartbeats — ALP CPU confirmed alive
    ALERT_ACTIVE,       // Laser detected — heartbeat byte1=01 or 98 trigger
    NOISE_WINDOW,       // Speaker alert active — UART data is glitch noise
    TEARDOWN,           // Register cleanup after alert, returning to idle
};

const char* alpStateName(AlpState s);

// ── Known gun fingerprints ───────────────────────────────────────────
// Gun ID from CX 00 YY frames: byte0 = gun family, byte2 = gun code.

enum class AlpGunType : uint8_t {
    UNKNOWN = 0,
    PL3_PROLITE,        // byte0=c8 gunCode=d5
    DRAGONEYE_COMPACT,  // byte0=c8 gunCode=d6
    LTI_TRUSPEED_LR,   // byte0=c9 gunCode=f5
    LASER_ATLANTA_PL2,  // byte0=cb gunCode=eb  (238 pps)
    MARKSMAN_ULTRALYTE, // jam: cd/d6 | observe: cd/0c
    STALKER_LZ1,        // byte0=cd gunCode=eb  (~130 pps)
    LASER_ALLY,         // byte0=cd gunCode=d7
    ATLANTA_STEALTH,    // byte0=ce gunCode=eb  (~68 pps "stealth mode")
};

const char* alpGunName(AlpGunType gun);
const char* alpGunAbbrev(AlpGunType gun);

// ── Gun lookup ───────────────────────────────────────────────────────

struct AlpGunCode {
    uint8_t byte0;      // Gun family (CX frame byte0)
    uint8_t gunCode;    // Gun identifier (CX frame byte2)
    AlpGunType gun;
};

AlpGunType alpLookupGun(uint8_t byte0, uint8_t gunCode);
AlpGunType alpLookupGunObserve(uint8_t byte0, uint8_t byte1);

// ── Checksum validation ─────────────────────────────────────────────
// All ALP frames: 4 bytes, checksum = (byte0 + byte1 + byte2) & 0x7F.

static inline uint8_t alpChecksum(uint8_t b0, uint8_t b1, uint8_t b2) {
    return (b0 + b1 + b2) & 0x7F;
}

static inline bool alpValidateChecksum(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs) {
    return cs == alpChecksum(b0, b1, b2);
}

// ── Snapshot for external consumers ──────────────────────────────────

struct AlpStatus {
    AlpState state;
    AlpGunType lastGun;
    uint32_t lastGunTimestampMs;    // millis() when gun was identified
    uint32_t lastHeartbeatMs;       // millis() of most recent valid frame
    uint32_t statusBurstCount;      // lifetime alert trigger count
    uint32_t heartbeatCount;        // lifetime heartbeat count
    uint32_t frameErrors;           // lifetime framing / checksum errors
    uint32_t noiseWindowCount;      // lifetime noise window entries
    uint8_t lastHbByte1;            // most recent B0 heartbeat byte1 (01=alert, 02-04=idle)
    bool uartActive;                // true if UART has received any data
};

// ── Alert Session ────────────────────────────────────────────────────
//
// A session is a single laser engagement from onset to final clear.
// It is the V1-shape projection of the parser state: the display
// consumer asks "is there a laser event?" and "which gun?" and gets
// two values — mirroring the V1's "are there alerts?" / "priority
// alert?" contract. All state-machine and frame-level complexity
// stays inside the module.
//
// Lifecycle:
//   open   — LISTENING|IDLE → ALERT_ACTIVE (fresh engagement)
//   stays open — across TEARDOWN↔ALERT_ACTIVE cycling (in-engagement
//                re-arms driven by byte1 01↔02 or repeat 98 triggers).
//                The gun frame arrives once at the opening of an
//                engagement; persisting the session across re-arms
//                keeps the gun abbreviation on the display for the
//                full event rather than flashing on/off.
//   close  — TEARDOWN → LISTENING (real end), or heartbeat timeout
//            (ALP went silent) with a session still open.
//
// Self-test flag:
//   Set when a session opens inside the 35s boot envelope AND an
//   F0/A8 preamble was seen within the first 5s of module uptime.
//   Cleared automatically if the session identifies a real gun —
//   self-test sequences never produce gun IDs, so any gun-identified
//   session is real by definition. While the flag is set, the V1-shape
//   accessors (hasLaserEvent, isLaserDetecting) return false so the
//   display stays clean through the ALP self-test window.

struct AlertSession {
    bool active = false;                            // session open?
    bool isSelfTest = false;                        // suppressed from display
    uint32_t startMs = 0;                           // session opened
    uint32_t endMs = 0;                             // 0 while active
    AlpGunType gun = AlpGunType::UNKNOWN;           // session's identified gun
    uint32_t gunIdentifiedMs = 0;                   // 0 if not yet identified
    uint32_t triggerCount = 0;                      // 98 frames in this session
    uint32_t rearmCount = 0;                        // TEARDOWN→ALERT cycles within session
};

// ── Module ───────────────────────────────────────────────────────────

class AlpRuntimeModule {
public:
    // GPIO pins — RX only (CPU TX on RJ-45 pin 2 → GPIO 1)
    // GPIO 2 reserved for future use (control pad RX / TX to ALP CPU)
    // GPIO 3 unassigned (no TX needed — receive-only listener)
    static constexpr int ALP_RX_PIN = 1;

    // Protocol constants — all frames are 4 bytes with 7-bit checksum
    static constexpr uint32_t ALP_BAUD = 19200;
    static constexpr size_t FRAME_LEN = 4;  // byte0 byte1 byte2 checksum

    // Alert trigger frame: 98 00 E3 7B
    static constexpr uint8_t ALERT_BYTE0 = 0x98;
    static constexpr uint8_t ALERT_BYTE1 = 0x00;
    static constexpr uint8_t ALERT_BYTE2 = 0xE3;

    // Heartbeat byte0 values
    static constexpr uint8_t HEARTBEAT_SINGLE_0 = 0xB0;
    static constexpr uint8_t HEARTBEAT_PAIRED_0 = 0xB8;
    static constexpr uint8_t HEARTBEAT_TRIPLE_0 = 0xE0;

    // Other known byte0 values
    static constexpr uint8_t DISCOVERY_BYTE0 = 0x91;
    static constexpr uint8_t SETUP_BYTE0_A8  = 0xA8;
    static constexpr uint8_t SETUP_BYTE0_F0  = 0xF0;

    // Timing thresholds
    static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 3000;
    static constexpr uint32_t NOISE_WINDOW_MAX_MS = 35000;  // 31s max + margin
    static constexpr uint32_t TEARDOWN_TIMEOUT_MS = 5000;
    static constexpr uint32_t ALERT_ACTIVE_TIMEOUT_MS = 15000;  // no 98 trigger rearm in 15s → teardown
    static constexpr size_t UART_RX_BUFFER_SIZE = 512;

    // Heartbeat byte1 alert detection: byte1=01 means laser detected,
    // byte1=02/03/04 means idle. This is the universal alert indicator
    // that works in both jam mode and observe (detect-only) mode.
    static constexpr uint8_t HB_BYTE1_ALERT = 0x01;

    // Noise detection: consecutive bad checksums to enter NOISE_WINDOW.
    // Applies from ALERT_ACTIVE or LISTENING (speaker noise in both modes).
    static constexpr uint32_t NOISE_CHECKSUM_THRESHOLD = 8;

    // RESYNC log throttle: only log every Nth bad checksum outside NOISE_WINDOW
    static constexpr uint32_t RESYNC_LOG_INTERVAL = 16;

    // Self-test envelope — see AlertSession comment for full rationale.
    // At ALP cold boot, the CPU runs a ~32s self-test that emits real
    // 98 02 00 triggers. An F0/A8 preamble within 5s of first frame is
    // pathognomonic; any alert session that opens within 35s of first
    // frame while the preamble has been observed is flagged as a
    // self-test. A gun identification in that session clears the flag
    // (self-test bursts never produce gun IDs).
    static constexpr uint32_t SELF_TEST_PREAMBLE_WINDOW_MS = 5000;
    static constexpr uint32_t SELF_TEST_ENVELOPE_MS = 35000;

    /**
     * Initialize the module.
     * @param enabled  true to open UART2 and begin listening
     * @param sdLogger  optional SD logger (nullptr disables logging)
     */
    void begin(bool enabled, AlpSdLogger* sdLogger = nullptr);

    /**
     * Called every main loop iteration.
     * Drains UART2 RX buffer, advances state machine.
     * @param nowMs  current millis()
     */
    void process(uint32_t nowMs);

    /**
     * Thread-safe snapshot of current state for display / API consumers.
     */
    AlpStatus snapshot() const;

    /** Current state (for wiring / guards). */
    AlpState getState() const { return state_; }

    /** Is module enabled? */
    bool isEnabled() const { return enabled_; }

    /** Is an alert currently active (gun identified, jamming in progress)? */
    bool isAlertActive() const {
        return state_ == AlpState::ALERT_ACTIVE ||
               state_ == AlpState::NOISE_WINDOW;
    }

    /** Most recently identified gun (persists across alerts). */
    AlpGunType lastIdentifiedGun() const { return lastGun_; }

    /** Timestamp of last gun identification. */
    uint32_t lastGunTimestampMs() const { return lastGunTimestampMs_; }

    // ── V1-shape display projection ──────────────────────────────────
    //
    // These are the two questions the display should ask. Everything
    // else (state machine, TEARDOWN, self-test windowing, re-arm
    // cycles) is parser-internal and should not leak into consumers.

    /**
     * Is there a laser event that should be shown on the display?
     * True from session open through final teardown. False during
     * self-test windows and when no engagement is active.
     */
    bool hasLaserEvent() const {
        return session_.active && !session_.isSelfTest;
    }

    /**
     * Is the ALP actively receiving alert frames right now (narrower
     * than hasLaserEvent — excludes the post-alert TEARDOWN "rescan"
     * window). Use for synthetic-alert injection into the V1 pipeline
     * where we only want to force LIVE mode during active detection.
     */
    bool isLaserDetecting() const {
        return hasLaserEvent() &&
               (state_ == AlpState::ALERT_ACTIVE ||
                state_ == AlpState::NOISE_WINDOW);
    }

    /**
     * The gun associated with the current laser event. Returns
     * UNKNOWN if no session is active OR the session has not yet
     * identified a gun. Does NOT leak lastGun_ between engagements.
     */
    AlpGunType eventGun() const {
        return session_.active ? session_.gun : AlpGunType::UNKNOWN;
    }

    /** Full current session for diagnostics / tests. */
    const AlertSession& currentSession() const { return session_; }

    /**
     * Does the ALP own the laser display right now? True when ALP is
     * enabled and the parser is in any state that indicates the module
     * is connected and producing data (i.e., anything except OFF or
     * IDLE). When true, the display pipeline suppresses V1 Gen2's
     * laser alerts so the ALP is the single authority on laser
     * rendering. When false — ALP disabled, UART gone quiet long
     * enough to drift to IDLE, or module never started — V1 laser
     * alerts pass through normally as a fallback.
     *
     * The V1 Gen2 unit and the ALP hardware both emit their own audio
     * for laser alerts via their built-in speakers, so v1_simple does
     * not need to duplicate the laser channel. Suppressing it when ALP
     * is alive eliminates the "ghost LASER tail" that used to appear
     * after an ALP engagement closed while V1's alert-persistence held
     * a duplicate visual.
     */
    bool ownsLaserDisplay() const {
        return enabled_ &&
               state_ != AlpState::OFF &&
               state_ != AlpState::IDLE;
    }

#ifdef UNIT_TEST
    // ── Test instrumentation ─────────────────────────────────────────
    void testInjectBytes(const uint8_t* data, size_t len);
    void testSetState(AlpState s) { state_ = s; }
    void testSetLastHeartbeat(uint32_t ms) { lastHeartbeatMs_ = ms; }
    AlpState testGetState() const { return state_; }
    uint32_t testGetHeartbeatCount() const { return heartbeatCount_; }
    uint32_t testGetStatusBurstCount() const { return statusBurstCount_; }
    uint32_t testGetFrameErrors() const { return frameErrors_; }
    uint32_t testGetNoiseWindowCount() const { return noiseWindowCount_; }
    uint8_t testGetLastHbByte1() const { return lastHbByte1_; }
    bool testGetAlertDetectedViaHb() const { return alertDetectedViaHb_; }
    const uint8_t* testGetRingBuf() const { return ringBuf_; }
    size_t testGetRingLen() const { return ringLen_; }
    // Session / self-test instrumentation
    uint32_t testGetFirstFrameMs() const { return firstFrameMs_; }
    uint32_t testGetSelfTestPreambleMs() const { return selfTestPreambleMs_; }
    void testSetFirstFrameMs(uint32_t ms) { firstFrameMs_ = ms; }
    void testSetSelfTestPreambleMs(uint32_t ms) { selfTestPreambleMs_ = ms; }
#endif

private:
    // ── State ────────────────────────────────────────────────────────
    AlpSdLogger* sdLogger_ = nullptr;
    bool enabled_ = false;
    bool begun_ = false;
    AlpState state_ = AlpState::OFF;

    // Ring buffer for incoming UART bytes
    static constexpr size_t RING_CAPACITY = 64;
    uint8_t ringBuf_[RING_CAPACITY] = {};
    size_t ringLen_ = 0;

    // Protocol tracking
    AlpGunType lastGun_ = AlpGunType::UNKNOWN;
    uint32_t lastGunTimestampMs_ = 0;
    uint32_t lastHeartbeatMs_ = 0;
    uint32_t lastFrameMs_ = 0;
    uint32_t noiseWindowEntryMs_ = 0;
    uint32_t teardownEntryMs_ = 0;
    uint32_t lastAlertTriggerMs_ = 0;   // last 98 XX XX trigger frame timestamp
    uint8_t lastHbByte1_ = 0xFF;        // most recent B0 heartbeat byte1
    bool alertDetectedViaHb_ = false;    // true when byte1 transitioned to 01
    bool uartHasReceivedData_ = false;

    // Session + self-test tracking
    AlertSession session_;
    uint32_t firstFrameMs_ = 0;          // first valid frame after begin()
    uint32_t selfTestPreambleMs_ = 0;    // F0/A8 within 5s of firstFrameMs_; 0 = not seen

    // Counters
    uint32_t statusBurstCount_ = 0;
    uint32_t heartbeatCount_ = 0;
    uint32_t frameErrors_ = 0;
    uint32_t noiseWindowCount_ = 0;
    uint32_t consecutiveBadChecksums_ = 0;

    // ── Internal methods ─────────────────────────────────────────────
    void transitionTo(AlpState newState, uint32_t nowMs);
    void drainUart(uint32_t nowMs);
    void parseRingBuffer(uint32_t nowMs);
    bool tryParseFrame(uint32_t nowMs);
    void handleAlertFrame(uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleHeartbeatFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleGunCandidate(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleRegisterFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleDiscoveryFrame(uint8_t b1, uint8_t b2, uint32_t nowMs);
    void consumeBytes(size_t count);
    void handleNoiseWindowTimeout(uint32_t nowMs);
    void handleTeardownTimeout(uint32_t nowMs);
    void handleHeartbeatTimeout(uint32_t nowMs);
    void handleAlertActiveTimeout(uint32_t nowMs);
};
