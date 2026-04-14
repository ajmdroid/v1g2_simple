/**
 * ALP Runtime Module — Unit Tests
 *
 * Tests gun identification parsing, state machine transitions,
 * heartbeat detection, timeout handling, and checksum validation.
 *
 * All test data uses the 4-byte frame protocol with 7-bit checksum:
 *   checksum = (byte0 + byte1 + byte2) & 0x7F
 *
 * Uses testInjectBytes() to feed raw protocol data without a real UART.
 */

#include <unity.h>

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/alp/alp_sd_logger.h"
#include "../../src/modules/alp/alp_sd_logger.cpp"
#include "../../src/modules/alp/alp_runtime_module.cpp"

// ── Helpers ──────────────────────────────────────────────────────────

static void resetModule() {
    alpRuntimeModule = AlpRuntimeModule();
}

static void beginEnabled() {
    alpRuntimeModule.begin(true);
}

static void beginDisabled() {
    alpRuntimeModule.begin(false);
}

static void inject(const uint8_t* data, size_t len) {
    alpRuntimeModule.testInjectBytes(data, len);
}

static void processAt(uint32_t ms) {
    alpRuntimeModule.process(ms);
}

// ── Alert burst test data (6 x 4-byte frames = 24 bytes) ───────────
// Each frame: byte0 byte1 byte2 checksum
// Alert trigger: 98 00 E3 7B
// Gun fingerprint: CX 00 YY checksum (Frame 4 of the burst)

// PL3 ProLite: byte0=c8, gunCode=d5
static const uint8_t BURST_PL3[] = {
    0x98, 0x00, 0xE3, 0x7B,    // Alert trigger
    0xC8, 0x04, 0xFA, 0x46,    // Static config
    0xC9, 0x00, 0xEC, 0x35,    // Pre-jam marker
    0xC8, 0x00, 0xD5, 0x1D,    // GUN — PL3 ProLite
    0xCA, 0x00, 0xEC, 0x36,    // Constant
    0xC9, 0x46, 0x65, 0x74,    // Mid-jam marker
};

// DragonEye Compact: byte0=c8, gunCode=d6
static const uint8_t BURST_DRAGONEYE[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xC8, 0x00, 0xD6, 0x1E,    // GUN — DragonEye Compact
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// LTI TruSpeed LR: byte0=c9, gunCode=f5
static const uint8_t BURST_TRUSPEED[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xC9, 0x00, 0xF5, 0x3E,    // GUN — LTI TruSpeed LR
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Marksman Ultralyte (jam mode): byte0=cd, gunCode=d6
static const uint8_t BURST_ULTRALYTE[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCD, 0x00, 0xD6, 0x23,    // GUN — Marksman Ultralyte (jam)
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Marksman Ultralyte (observe mode): fingerprint is CD 0C 00 → (byte0=cd, byte1=0c)
// From live capture: Ultralyte fire, ALP in observe (detect-only) mode, April 2026
static const uint8_t BURST_ULTRALYTE_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCD, 0x0C, 0x00, 0x59,    // GUN — Ultralyte (observe): (CD, 0C)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// PL3 ProLite (observe mode): fingerprint is C8 0D 00 → (byte0=c8, byte1=0d)
// From live capture: PL3 fire, ALP in observe mode, April 2026
static const uint8_t BURST_PL3_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xC8, 0x0D, 0x00, 0x55,    // GUN — PL3 ProLite (observe): (C8, 0D)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// LTI TruSpeed LR (observe mode): fingerprint is C9 0E 00 → (byte0=c9, byte1=0e)
// From live capture: TruSpeed fire, ALP in observe mode, April 2026
static const uint8_t BURST_TRUSPEED_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xC9, 0x0E, 0x00, 0x57,    // GUN — TruSpeed LR (observe): (C9, 0E)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Laser Atlanta PL2 (observe mode): fingerprint is CB 10 00 → (byte0=cb, byte1=10)
// From live capture: PL2 fire, ALP in observe mode, April 2026
static const uint8_t BURST_PL2_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCB, 0x10, 0x00, 0x5B,    // GUN — Laser Atlanta PL2 (observe): (CB, 10)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Stalker LZ1 (observe mode): fingerprint is CD 0D 00 → (byte0=cd, byte1=0d)
// From live capture: Stalker LZ1 fire, ALP in observe mode, April 2026
static const uint8_t BURST_STALKER_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCD, 0x0D, 0x00, 0x5A,    // GUN — Stalker LZ1 (observe): (CD, 0D)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// DragonEye Compact (observe mode): fingerprint is C8 11 00 → (byte0=c8, byte1=11)
// From live capture: DragonEye Compact fire, ALP in observe mode, April 2026
static const uint8_t BURST_DRAGONEYE_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xC8, 0x11, 0x00, 0x59,    // GUN — DragonEye Compact (observe): (C8, 11)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Laser Ally (observe mode): fingerprint is CD 10 00 → (byte0=cd, byte1=10)
// From live capture: Laser Ally fire, ALP in observe mode, April 2026
static const uint8_t BURST_LASER_ALLY_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCD, 0x10, 0x00, 0x5D,    // GUN — Laser Ally (observe): (CD, 10)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Atlanta Stealth (observe mode): fingerprint is CE 0C 00 → (byte0=ce, byte1=0c)
// From live capture: Atlanta Stealth fire, ALP in observe mode, April 2026
static const uint8_t BURST_ATLANTA_OBS[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (observe mode)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCE, 0x0C, 0x00, 0x5A,    // GUN — Atlanta Stealth (observe): (CE, 0C)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Stalker LZ1: byte0=cd, gunCode=eb
static const uint8_t BURST_STALKER[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCD, 0x00, 0xEB, 0x38,    // GUN — Stalker LZ1
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Atlanta Stealth: byte0=ce, gunCode=eb
static const uint8_t BURST_ATLANTA[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCE, 0x00, 0xEB, 0x39,    // GUN — Atlanta Stealth
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Laser Ally: byte0=cd, gunCode=d7
static const uint8_t BURST_LASER_ALLY[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCD, 0x00, 0xD7, 0x24,    // GUN — Laser Ally
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Laser Atlanta PL2: byte0=cb, gunCode=eb
static const uint8_t BURST_PL2[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCB, 0x00, 0xEB, 0x36,    // GUN — Laser Atlanta PL2
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Unknown gun: ff 00 ff (unknown byte0+gunCode)
static const uint8_t BURST_UNKNOWN[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xFF, 0x00, 0xFF, 0x7E,    // GUN — Unknown (valid checksum)
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Heartbeat: single B0 00 E6 checksum
static const uint8_t HEARTBEAT_SINGLE[] = { 0xB0, 0x00, 0xE6, 0x16 };

// Heartbeat: paired B8 00 FE checksum
static const uint8_t HEARTBEAT_PAIRED[] = { 0xB8, 0x00, 0xFE, 0x36 };

// Discovery poll: 91 00 12 checksum
static const uint8_t DISCOVERY_POLL[] = { 0x91, 0x00, 0x12, 0x23 };

// Register write with FD terminator at byte2: D0 00 FD checksum
static const uint8_t REG_WRITE_FD[] = { 0xD0, 0x00, 0xFD, 0x4D };

// Register write with FD at byte2 (D3 variant): D3 00 FD checksum
static const uint8_t REG_WRITE_FD_D3[] = { 0xD3, 0x00, 0xFD, 0x50 };

// ── Test setup/teardown ──────────────────────────────────────────────

void setUp() {
    resetModule();
}

void tearDown() {}

// ── Checksum validation tests ────────────────────────────────────────

void test_checksum_calculation() {
    // Known good: B0 02 00 → checksum 0x32
    TEST_ASSERT_EQUAL(0x32, alpChecksum(0xB0, 0x02, 0x00));
    // Known good: 98 00 E3 → checksum 0x7B
    TEST_ASSERT_EQUAL(0x7B, alpChecksum(0x98, 0x00, 0xE3));
    // Known good: B0 01 00 → checksum 0x31
    TEST_ASSERT_EQUAL(0x31, alpChecksum(0xB0, 0x01, 0x00));
    // Known good: C9 1A 04 → checksum 0x67
    TEST_ASSERT_EQUAL(0x67, alpChecksum(0xC9, 0x1A, 0x04));
}

void test_checksum_validation_pass() {
    TEST_ASSERT_TRUE(alpValidateChecksum(0xB0, 0x02, 0x00, 0x32));
    TEST_ASSERT_TRUE(alpValidateChecksum(0x98, 0x00, 0xE3, 0x7B));
    TEST_ASSERT_TRUE(alpValidateChecksum(0x91, 0x00, 0x12, 0x23));
}

void test_checksum_validation_fail() {
    TEST_ASSERT_FALSE(alpValidateChecksum(0xB0, 0x02, 0x00, 0x33));  // Wrong checksum
    TEST_ASSERT_FALSE(alpValidateChecksum(0x98, 0x00, 0xE3, 0x00));  // Zero checksum
    TEST_ASSERT_FALSE(alpValidateChecksum(0xFF, 0xFF, 0xFF, 0xFF));  // All 0xFF
}

// ── begin() tests ────────────────────────────────────────────────────

void test_begin_disabled_stays_disabled() {
    beginDisabled();
    TEST_ASSERT_EQUAL(AlpState::OFF, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.isEnabled());
}

void test_begin_enabled_goes_idle() {
    beginEnabled();
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.isEnabled());
}

void test_process_noop_when_disabled() {
    beginDisabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::OFF, alpRuntimeModule.getState());
}

// ── Gun lookup table tests ───────────────────────────────────────────

void test_gun_lookup_pl3() {
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpLookupGun(0xC8, 0xD5));
}

void test_gun_lookup_dragoneye() {
    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpLookupGun(0xC8, 0xD6));
}

void test_gun_lookup_truspeed() {
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpLookupGun(0xC9, 0xF5));
}

void test_gun_lookup_pl2() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpLookupGun(0xCB, 0xEB));
}

void test_gun_lookup_ultralyte() {
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpLookupGun(0xCD, 0xD6));
}

// Observe-mode gun lookups — (byte0, byte1) fingerprint from CX YY 00 frames
void test_gun_lookup_ultralyte_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpLookupGunObserve(0xCD, 0x0C));
}

void test_gun_lookup_pl3_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpLookupGunObserve(0xC8, 0x0D));
}

void test_gun_lookup_pl2_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpLookupGunObserve(0xCB, 0x10));
}

void test_gun_lookup_truspeed_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpLookupGunObserve(0xC9, 0x0E));
}

void test_gun_lookup_dragoneye_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpLookupGunObserve(0xC8, 0x11));
}

void test_gun_lookup_laser_ally_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpLookupGunObserve(0xCD, 0x10));
}

void test_gun_lookup_atlanta_stealth_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpLookupGunObserve(0xCE, 0x0C));
}

void test_gun_lookup_stalker_observe() {
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpLookupGunObserve(0xCD, 0x0D));
}

void test_gun_lookup_observe_unknown() {
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpLookupGunObserve(0xFF, 0xFF));
}

void test_gun_lookup_stalker() {
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpLookupGun(0xCD, 0xEB));
}

void test_gun_lookup_laser_ally() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpLookupGun(0xCD, 0xD7));
}

void test_gun_lookup_atlanta_stealth() {
    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpLookupGun(0xCE, 0xEB));
}

void test_gun_lookup_unknown() {
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpLookupGun(0xFF, 0xFF));
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpLookupGun(0x00, 0x00));
}

// gunCode collision resolution: cd d6 = Ultralyte, c8 d6 = DragonEye
void test_gun_guncode_collision_d6_resolved_by_byte0() {
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpLookupGun(0xCD, 0xD6));
    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpLookupGun(0xC8, 0xD6));
}

// gunCode collision: cb eb = PL2, cd eb = Stalker, ce eb = Atlanta Stealth
void test_gun_guncode_collision_eb_resolved_by_byte0() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpLookupGun(0xCB, 0xEB));
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpLookupGun(0xCD, 0xEB));
    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpLookupGun(0xCE, 0xEB));
}

// ── Alert burst parsing (all 8 guns) ────────────────────────────────

void test_burst_identifies_pl3() {
    beginEnabled();
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(1000u, alpRuntimeModule.lastGunTimestampMs());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_burst_identifies_dragoneye() {
    beginEnabled();
    inject(BURST_DRAGONEYE, sizeof(BURST_DRAGONEYE));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_truspeed() {
    beginEnabled();
    inject(BURST_TRUSPEED, sizeof(BURST_TRUSPEED));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_ultralyte() {
    beginEnabled();
    inject(BURST_ULTRALYTE, sizeof(BURST_ULTRALYTE));
    processAt(4000);

    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_ultralyte_observe() {
    beginEnabled();
    inject(BURST_ULTRALYTE_OBS, sizeof(BURST_ULTRALYTE_OBS));
    processAt(4500);

    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_truspeed_observe() {
    beginEnabled();
    inject(BURST_TRUSPEED_OBS, sizeof(BURST_TRUSPEED_OBS));
    processAt(4700);

    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_dragoneye_observe() {
    beginEnabled();
    inject(BURST_DRAGONEYE_OBS, sizeof(BURST_DRAGONEYE_OBS));
    processAt(5200);

    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_laser_ally_observe() {
    beginEnabled();
    inject(BURST_LASER_ALLY_OBS, sizeof(BURST_LASER_ALLY_OBS));
    processAt(5100);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_atlanta_stealth_observe() {
    beginEnabled();
    inject(BURST_ATLANTA_OBS, sizeof(BURST_ATLANTA_OBS));
    processAt(5000);

    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_stalker_observe() {
    beginEnabled();
    inject(BURST_STALKER_OBS, sizeof(BURST_STALKER_OBS));
    processAt(4900);

    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_pl2_observe() {
    beginEnabled();
    inject(BURST_PL2_OBS, sizeof(BURST_PL2_OBS));
    processAt(4800);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_pl3_observe() {
    beginEnabled();
    inject(BURST_PL3_OBS, sizeof(BURST_PL3_OBS));
    processAt(4600);

    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_stalker() {
    beginEnabled();
    inject(BURST_STALKER, sizeof(BURST_STALKER));
    processAt(5000);

    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_atlanta() {
    beginEnabled();
    inject(BURST_ATLANTA, sizeof(BURST_ATLANTA));
    processAt(6000);

    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_laser_ally() {
    beginEnabled();
    inject(BURST_LASER_ALLY, sizeof(BURST_LASER_ALLY));
    processAt(7000);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_pl2() {
    beginEnabled();
    inject(BURST_PL2, sizeof(BURST_PL2));
    processAt(8000);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_unknown_gun_still_alerts() {
    beginEnabled();
    inject(BURST_UNKNOWN, sizeof(BURST_UNKNOWN));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Heartbeat parsing ────────────────────────────────────────────────

void test_heartbeat_transitions_idle_to_listening() {
    beginEnabled();
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());

    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetHeartbeatCount());
}

void test_paired_heartbeat_counted() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    inject(HEARTBEAT_PAIRED, sizeof(HEARTBEAT_PAIRED));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetHeartbeatCount());
}

void test_discovery_poll_transitions_to_listening() {
    beginEnabled();
    inject(DISCOVERY_POLL, sizeof(DISCOVERY_POLL));
    processAt(500);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Heartbeat timeout ────────────────────────────────────────────────

void test_heartbeat_timeout_returns_to_idle() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    processAt(1000 + AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
}

void test_heartbeat_keeps_listening_if_within_timeout() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    processAt(1000 + AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS - 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Register write with FD terminator (teardown trigger) ─────────────

void test_fd_terminator_triggers_teardown_from_alert() {
    beginEnabled();

    // Enter ALERT_ACTIVE via alert burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Register write with FD terminator at byte2
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

void test_fd_terminator_d3_triggers_teardown() {
    beginEnabled();

    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // D3 variant with FD at byte2
    inject(REG_WRITE_FD_D3, sizeof(REG_WRITE_FD_D3));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

// ── Teardown timeout ─────────────────────────────────────────────────

void test_teardown_timeout_returns_to_listening() {
    beginEnabled();

    alpRuntimeModule.testSetState(AlpState::TEARDOWN);
    alpRuntimeModule.testSetLastHeartbeat(1000);

    processAt(1000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Checksum-based resync ───────────────────────────────────────────

void test_resync_discards_garbage_before_heartbeat() {
    beginEnabled();

    // 1 garbage byte + valid 4-byte heartbeat
    const uint8_t data[] = { 0xFF, 0xB0, 0x00, 0xE6, 0x16 };
    inject(data, sizeof(data));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetFrameErrors() > 0);
}

void test_resync_discards_garbage_before_alert_burst() {
    beginEnabled();

    // 2 garbage bytes + full 24-byte alert burst
    uint8_t data[2 + 24];
    data[0] = 0xAA;
    data[1] = 0xBB;
    memcpy(data + 2, BURST_PL3, 24);
    inject(data, sizeof(data));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());
}

// ── Bad checksum rejection ──────────────────────────────────────────

void test_bad_checksum_frame_rejected() {
    beginEnabled();

    // Frame with valid-looking bytes but wrong checksum
    const uint8_t bad[] = { 0xB0, 0x02, 0x00, 0x99 };  // correct cs would be 0x32
    inject(bad, sizeof(bad));
    processAt(1000);

    // Should still be IDLE — frame was rejected
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetHeartbeatCount());
}

// ── Noise window via consecutive bad checksums ──────────────────────

void test_consecutive_bad_checksums_trigger_noise_window() {
    beginEnabled();

    // First get to ALERT_ACTIVE
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Inject enough noise bytes to trigger NOISE_WINDOW
    // Need NOISE_CHECKSUM_THRESHOLD consecutive bad checksums
    // Each bad 4-byte group consumes 1 byte, so we need enough bytes
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) {
        noise[i] = 0xFF;  // All 0xFF won't pass checksum (sum=0x2FD, cs=0x7D, but byte3=0xFF)
    }
    inject(noise, sizeof(noise));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetNoiseWindowCount());
}

void test_noise_window_ends_on_valid_frame() {
    beginEnabled();

    // Force into NOISE_WINDOW
    alpRuntimeModule.testSetState(AlpState::NOISE_WINDOW);
    alpRuntimeModule.testSetLastHeartbeat(1000);

    // Inject valid heartbeat — should exit noise, enter teardown
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

// ── Snapshot ─────────────────────────────────────────────────────────

void test_snapshot_reflects_state() {
    beginEnabled();
    inject(BURST_STALKER, sizeof(BURST_STALKER));
    processAt(5000);

    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, status.state);
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, status.lastGun);
    TEST_ASSERT_EQUAL(5000u, status.lastGunTimestampMs);
    TEST_ASSERT_EQUAL(1u, status.statusBurstCount);
    TEST_ASSERT_TRUE(status.uartActive);
}

void test_snapshot_default_values() {
    beginEnabled();
    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL(AlpState::IDLE, status.state);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, status.lastGun);
    TEST_ASSERT_EQUAL(0u, status.statusBurstCount);
    TEST_ASSERT_EQUAL(0u, status.heartbeatCount);
    TEST_ASSERT_FALSE(status.uartActive);
}

// ── isAlertActive() ──────────────────────────────────────────────────

void test_is_alert_active_during_alert() {
    beginEnabled();
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_TRUE(alpRuntimeModule.isAlertActive());
}

void test_is_not_alert_active_during_listening() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_FALSE(alpRuntimeModule.isAlertActive());
}

// ── Gun name strings ─────────────────────────────────────────────────

void test_gun_names_not_null() {
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::UNKNOWN));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::PL3_PROLITE));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::DRAGONEYE_COMPACT));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::LTI_TRUSPEED_LR));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::LASER_ATLANTA_PL2));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::MARKSMAN_ULTRALYTE));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::STALKER_LZ1));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::LASER_ALLY));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::ATLANTA_STEALTH));
}

void test_state_names_not_null() {
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::OFF));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::IDLE));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::LISTENING));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::ALERT_ACTIVE));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::NOISE_WINDOW));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::TEARDOWN));
}

// ── Multiple alert bursts ───────────────────────────────────────────

void test_sequential_bursts_update_gun() {
    beginEnabled();

    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());

    inject(BURST_ULTRALYTE, sizeof(BURST_ULTRALYTE));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Full lifecycle: heartbeat → burst → teardown → listening ─────────

void test_full_alert_lifecycle() {
    beginEnabled();

    // 1. Heartbeats → LISTENING
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // 2. Alert burst → ALERT_ACTIVE + gun ID
    inject(BURST_TRUSPEED, sizeof(BURST_TRUSPEED));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());

    // 3. FD terminator → TEARDOWN
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());

    // 4. Teardown timeout → LISTENING
    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Alert trigger as standalone frame ────────────────────────────────

void test_alert_trigger_standalone() {
    beginEnabled();

    // Just the alert trigger frame, no burst following
    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
    // No gun identified yet — gun comes in a later frame
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
}

// ── Observe-mode alert trigger (98 02 00) ───────────────────────────

void test_observe_mode_alert_98_02_00() {
    beginEnabled();

    // Observe-mode alert: 98 02 00 (checksum = (98+02+00)&7F = 1A)
    const uint8_t observe_alert[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(observe_alert, sizeof(observe_alert));
    processAt(1000);

    // Should transition to ALERT_ACTIVE — observe mode alert trigger
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_observe_mode_rearm_increments_burst_count() {
    beginEnabled();

    // Already in ALERT_ACTIVE from jam trigger
    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // 98 02 00 while already ALERT_ACTIVE — re-arm counts as a new burst
    const uint8_t observe[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(observe, sizeof(observe));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Other 98 XX YY status frames (not alert triggers) ───────────────

void test_status_frame_98_other() {
    beginEnabled();

    // Some other 98 frame that isn't 00 E3 or 02 00
    // 98 04 10 — checksum = (98+04+10)&7F = 2C
    const uint8_t status[] = { 0x98, 0x04, 0x10, 0x2C };
    inject(status, sizeof(status));
    processAt(1000);

    // Should transition to LISTENING (not ALERT_ACTIVE)
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Heartbeat byte1 alert detection ─────────────────────────────────

void test_heartbeat_byte1_alert_transitions_to_alert_active() {
    beginEnabled();

    // Get to LISTENING with an idle heartbeat first
    // B0 02 00 — checksum = (B0+02+00)&7F = 32
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // B0 01 00 — byte1=01 means laser detected
    // checksum = (B0+01+00)&7F = 31
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());
    TEST_ASSERT_EQUAL(0x01, alpRuntimeModule.testGetLastHbByte1());
}

void test_heartbeat_byte1_idle_resolves_alert() {
    beginEnabled();

    // Get to LISTENING
    const uint8_t hb_idle1[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle1, sizeof(hb_idle1));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Alert via heartbeat byte1=01
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Back to idle via heartbeat byte1=03
    // checksum = (B0+03+00)&7F = 33
    const uint8_t hb_idle2[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_idle2, sizeof(hb_idle2));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.testGetAlertDetectedViaHb());
}

void test_heartbeat_byte1_b8_does_not_trigger_alert() {
    beginEnabled();

    // B8 frames do NOT carry alert info — only B0 does
    // B8 01 00 — checksum = (B8+01+00)&7F = 39
    const uint8_t hb_b8[] = { 0xB8, 0x01, 0x00, 0x39 };
    inject(hb_b8, sizeof(hb_b8));
    processAt(1000);

    // Should be LISTENING (from IDLE), NOT ALERT_ACTIVE
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetStatusBurstCount());
    TEST_ASSERT_FALSE(alpRuntimeModule.testGetAlertDetectedViaHb());
}

void test_heartbeat_alert_repeated_01_no_double_trigger() {
    beginEnabled();

    // Idle heartbeat first
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    // First alert heartbeat
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // Second alert heartbeat (still 01) — should NOT increment burst count
    inject(hb_alert, sizeof(hb_alert));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_heartbeat_alert_then_noise_from_listening() {
    beginEnabled();

    // Idle heartbeat → LISTENING
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Alert heartbeat → ALERT_ACTIVE
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Flood of noise → NOISE_WINDOW
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = 0xFF;
    inject(noise, sizeof(noise));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetNoiseWindowCount());
}

void test_noise_from_listening_with_hb_alert() {
    beginEnabled();

    // Get to LISTENING with idle heartbeat
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Simulate observe mode: heartbeat byte1=01 sets flag, then immediate
    // noise flood (speaker alert) before we even process() again.
    // We need to put the alert heartbeat + noise in same inject.
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    // Process to register the alert heartbeat → ALERT_ACTIVE
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());

    // Now noise comes in
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = 0xFF;
    inject(noise, sizeof(noise));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
}

void test_snapshot_includes_hb_byte1() {
    beginEnabled();

    // Heartbeat with byte1=04
    // B0 04 00 — checksum = (B0+04+00)&7F = 34
    const uint8_t hb[] = { 0xB0, 0x04, 0x00, 0x34 };
    inject(hb, sizeof(hb));
    processAt(1000);

    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL(0x04, status.lastHbByte1);
}

void test_teardown_clears_alert_flag() {
    beginEnabled();

    // Alert via heartbeat
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());

    // Resolve via heartbeat byte1=02
    const uint8_t hb_idle2[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle2, sizeof(hb_idle2));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());

    // Teardown timeout → LISTENING, flag should be cleared
    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.testGetAlertDetectedViaHb());
}

// ── Gun cleared on new alert entry (stale gun fix) ─────────────

void test_new_alert_clears_stale_gun_via_98_trigger() {
    beginEnabled();

    // First alert: identify PL3 via burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());

    // Resolve: FD terminator → TEARDOWN → timeout → LISTENING
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    processAt(2000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Second alert: 98 trigger only — no gun frame follows
    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Gun must be UNKNOWN — the previous PL3 must not bleed through
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.lastGunTimestampMs());
}

void test_new_alert_clears_stale_gun_via_heartbeat() {
    beginEnabled();

    // First alert: identify TruSpeed via burst
    inject(BURST_TRUSPEED, sizeof(BURST_TRUSPEED));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());

    // Resolve via FD terminator → TEARDOWN → timeout → LISTENING
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    processAt(2000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Idle heartbeat to seed lastHbByte1 for the transition detection
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(9000);

    // Second alert via heartbeat byte1=01 — no gun frame
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Gun must be UNKNOWN — the previous TruSpeed must not bleed through
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
}

void test_new_alert_identifies_fresh_gun_after_clear() {
    beginEnabled();

    // First alert: identify PL3
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());

    // Resolve → LISTENING
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    processAt(2000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Second alert: full Ultralyte burst — should show Ultralyte, not PL3
    inject(BURST_ULTRALYTE, sizeof(BURST_ULTRALYTE));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
}

// Regression for alp_18.csv in-engagement teardown re-arm.
//
// Observed: PL2 identified at 36.768s via observe-mode burst → gun shown.
// At 46.352s the heartbeat byte1 dropped 01→02 (alert resolve-to-TEARDOWN)
// and — in the same millisecond — a 98 02 00 re-arm fired, pushing state
// back to ALERT_ACTIVE. Pre-fix, the TEARDOWN→ALERT_ACTIVE transitionTo
// wiped lastGun_, leaving the remaining ~20s of the same engagement
// showing generic "LASER" on the display.
//
// Fix: narrow the clear guard to (LISTENING|IDLE) → ALERT_ACTIVE only.
// Assertion: PL2 must still be identified after the re-arm.
void test_gun_persists_through_teardown_rearm_cycle() {
    beginEnabled();

    // Seed LISTENING with a 02 idle heartbeat (also seeds lastHbByte1_=02
    // so the later 01 transition registers as a real alert flip).
    const uint8_t hb_idle_02_a[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle_02_a, sizeof(hb_idle_02_a));
    processAt(8000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // First alert: observe-mode PL2 burst → ALERT_ACTIVE + PL2 identified.
    // (Fresh LISTENING → ALERT_ACTIVE transition; Rev 6 clear fires as
    // expected and is immediately re-populated by the CB 10 00 gun frame.)
    inject(BURST_PL2_OBS, sizeof(BURST_PL2_OBS));
    processAt(37000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2,
                      alpRuntimeModule.lastIdentifiedGun());

    // Within the same engagement: byte1 flips to 01 (alert confirm via HB).
    // State stays ALERT_ACTIVE — this is the "still firing" heartbeat.
    const uint8_t hb_alert_01[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert_01, sizeof(hb_alert_01));
    processAt(45000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // byte1 01→02: ALERT_ACTIVE → TEARDOWN (end-of-hold edge).
    const uint8_t hb_idle_02_b[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle_02_b, sizeof(hb_idle_02_b));
    processAt(46000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    // Gun must survive the ALERT_ACTIVE → TEARDOWN edge.
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2,
                      alpRuntimeModule.lastIdentifiedGun());

    // 98 02 00 re-fires during TEARDOWN → TEARDOWN → ALERT_ACTIVE (in-engagement
    // re-arm — this is the edge that used to wipe the gun).
    const uint8_t observe_trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(observe_trigger, sizeof(observe_trigger));
    processAt(46500);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // THE ASSERTION. Before the fix this was UNKNOWN.
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2,
                      alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(37000u, alpRuntimeModule.lastGunTimestampMs());
}

// ── AlertSession / V1-shape display projection ──────────────────────
//
// These tests validate the session layer that projects the parser's
// internal state machine into the two V1-shape accessors the display
// consumes: hasLaserEvent() and eventGun(). Bugs #1 (startup shows
// laser) and #3 (lastGun leaks on reset) are fixed at this layer.

void test_session_closed_by_default() {
    beginEnabled();
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.isLaserDetecting());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.eventGun());
}

// ── ownsLaserDisplay() — V1 laser suppression gate ──────────────────
//
// The display pipeline asks this one question: "should V1's BAND_LASER
// alerts be suppressed because ALP is handling laser?" Contract:
//   true  — ALP enabled AND connected (any state except OFF / IDLE)
//   false — ALP disabled, or state is OFF (never started) / IDLE (UART
//           timeout drifted the module into silence). V1 laser falls
//           through as a backup detection channel.

void test_owns_laser_display_false_when_disabled() {
    // Module never begun → OFF → does not own.
    TEST_ASSERT_FALSE(alpRuntimeModule.ownsLaserDisplay());

    // Explicit begin(disabled) → OFF → does not own.
    alpRuntimeModule.begin(false, nullptr);
    TEST_ASSERT_FALSE(alpRuntimeModule.ownsLaserDisplay());
}

void test_owns_laser_display_true_when_listening() {
    // Enabled + first valid heartbeat → LISTENING → owns.
    beginEnabled();
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.ownsLaserDisplay());
}

void test_owns_laser_display_true_during_alert() {
    beginEnabled();
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.ownsLaserDisplay());
}

void test_owns_laser_display_false_when_idle_after_timeout() {
    // LISTENING → IDLE via heartbeat timeout → no longer owns.
    // V1 laser should pass through because the ALP has gone silent.
    beginEnabled();
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_TRUE(alpRuntimeModule.ownsLaserDisplay());

    // Drift past the heartbeat watchdog — IDLE.
    processAt(1000 + AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS + 500);
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.ownsLaserDisplay());
}

void test_session_opens_on_fresh_alert_from_listening() {
    beginEnabled();

    // LISTENING via heartbeat
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());

    // Fresh alert: full PL3 burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_FALSE(s.isSelfTest);
    TEST_ASSERT_EQUAL(2000u, s.startMs);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, s.gun);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.isLaserDetecting());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.eventGun());
}

void test_session_survives_teardown_rearm_cycle() {
    // alp_18-shape: gun identified once, then an in-engagement TEARDOWN↔ALERT
    // cycle must NOT close the session or clear eventGun.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    inject(BURST_PL2_OBS, sizeof(BURST_PL2_OBS));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.eventGun());

    // byte1 01 → 02 drop: ALERT_ACTIVE → TEARDOWN
    const uint8_t hb_alert_01[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert_01, sizeof(hb_alert_01));
    processAt(3000);
    const uint8_t hb_idle_02[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle_02, sizeof(hb_idle_02));
    processAt(4000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    // Session still open during teardown; eventGun still bound.
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.eventGun());

    // Re-arm via 98 02 00 while in TEARDOWN
    const uint8_t observe_trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(observe_trigger, sizeof(observe_trigger));
    processAt(5000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.eventGun());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.currentSession().rearmCount);
}

void test_session_closes_on_teardown_to_listening() {
    // Full engagement: alert → teardown → timeout → LISTENING → session closed.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());

    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());

    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Session closed. Display accessors must return the "no event" values.
    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_TRUE(s.endMs > 0);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.isLaserDetecting());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.eventGun());

    // But lastIdentifiedGun() still reflects the protocol-level cache,
    // for SD logger / diagnostics. The display never reads this.
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.lastIdentifiedGun());
}

// Regression for bug #3: lastGun leaking onto the display during LISTENING.
// Before the session layer, the display read lastIdentifiedGun() directly
// and could render a stale gun name from the previous engagement. With
// session gating, eventGun() returns UNKNOWN between engagements even
// though lastGun_ persists internally.
void test_event_gun_unknown_between_engagements_even_with_stale_lastgun() {
    beginEnabled();

    // First engagement: identify PL3 then close cleanly.
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(3000);
    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // lastGun_ is still populated (protocol-level cache). Display must not
    // see it — eventGun() is gated on an active session.
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.eventGun());
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
}

// ── Self-test suppression (bug #1) ──────────────────────────────────

void test_self_test_flagged_when_preamble_in_window() {
    // Canonical cold-boot shape: first heartbeat, F0 preamble at +2s,
    // 98 02 00 trigger shortly after. The session must be flagged
    // self-test and suppressed from display.
    beginEnabled();

    // First valid frame: idle heartbeat. Sets firstFrameMs_ = 1000.
    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(1000u, alpRuntimeModule.testGetFirstFrameMs());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetSelfTestPreambleMs());

    // F0 preamble within the 5s window.
    // F0 03 00 — checksum = (F0+03+00)&7F = 73
    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(3000);
    TEST_ASSERT_EQUAL(3000u, alpRuntimeModule.testGetSelfTestPreambleMs());

    // Observe-mode alert trigger. Opens a session inside the envelope.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(3100);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_TRUE(s.isSelfTest);
    // Display sees nothing — this is the fix for bug #1.
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.isLaserDetecting());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.eventGun());
}

void test_self_test_unflagged_when_real_gun_identified() {
    // Safety release: a real gun ID during the self-test window must
    // un-declare the session as self-test. The ALP's self-test never
    // emits CX gun frames, so a gun ID is pathognomonic for real.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(3000);

    // Now a real PL2 observe-mode burst arrives inside the envelope.
    // The trigger opens a self-test-flagged session, then the CB 10 00
    // gun frame un-flags it.
    inject(BURST_PL2_OBS, sizeof(BURST_PL2_OBS));
    processAt(4000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_FALSE(s.isSelfTest);  // Released by gun ID
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, s.gun);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.eventGun());
}

void test_self_test_not_flagged_without_preamble() {
    // No F0/A8 ever seen → session is never self-test even at cold boot.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(2000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_FALSE(s.isSelfTest);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
}

void test_self_test_preamble_after_window_ignored() {
    // F0 arriving AFTER the 5s preamble window does not arm self-test.
    // (In practice F0 only ever arrives at cold boot; this guards
    // against spurious late F0 frames or unexpected protocol drift.)
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    // F0 at +6s — outside the 5s window.
    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(7000);
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetSelfTestPreambleMs());

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(8000);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isSelfTest);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
}

void test_self_test_envelope_expires_after_35s() {
    // Preamble observed at boot, but the alert trigger arrives after
    // the 35s envelope. Session must NOT be flagged self-test.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(3000);

    // Alert at +40s — envelope is 35s from firstFrameMs_ (1000) so
    // cutoff is 36000. 41000 > 36000 → not self-test.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(41000);

    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isSelfTest);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
}

// ── ALP SD logger CSV format regression tests ───────────────────────

void test_sd_logger_frame_columns_match_header() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logFrame(1234, "ALERT_OBS", 0x98, 0x02, 0x00, 0x1A, AlpState::LISTENING);

    TEST_ASSERT_EQUAL_STRING(
        "1234,ALERT_OBS,LISTENING,,98,02,00,1A,,\n",
        logger.testGetLastLine());
}

void test_sd_logger_heartbeat_includes_checksum() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logHeartbeat(5678, 0xB0, 0x03, 0x00, AlpState::ALERT_ACTIVE);

    TEST_ASSERT_EQUAL_STRING(
        "5678,HEARTBEAT,ALERT_ACTIVE,,B0,03,00,33,,\n",
        logger.testGetLastLine());
}

void test_sd_logger_gun_id_jam_preserves_raw_frame() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logGunIdentified(42, AlpGunType::MARKSMAN_ULTRALYTE,
                            0xCD, 0xD6, false, AlpState::ALERT_ACTIVE);

    TEST_ASSERT_EQUAL_STRING(
        "42,GUN_ID,ALERT_ACTIVE,,CD,00,D6,23,Marksman Ultralyte,jam\n",
        logger.testGetLastLine());
}

void test_sd_logger_gun_id_observe_preserves_raw_frame() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logGunIdentified(43, AlpGunType::PL3_PROLITE,
                            0xC8, 0x0D, true, AlpState::ALERT_ACTIVE);

    TEST_ASSERT_EQUAL_STRING(
        "43,GUN_ID,ALERT_ACTIVE,,C8,0D,00,55,PL3 ProLite,observe\n",
        logger.testGetLastLine());
}

// ── Runner ───────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Checksum tests
    RUN_TEST(test_checksum_calculation);
    RUN_TEST(test_checksum_validation_pass);
    RUN_TEST(test_checksum_validation_fail);

    // begin() tests
    RUN_TEST(test_begin_disabled_stays_disabled);
    RUN_TEST(test_begin_enabled_goes_idle);
    RUN_TEST(test_process_noop_when_disabled);

    // Gun lookup table
    RUN_TEST(test_gun_lookup_pl3);
    RUN_TEST(test_gun_lookup_dragoneye);
    RUN_TEST(test_gun_lookup_truspeed);
    RUN_TEST(test_gun_lookup_pl2);
    RUN_TEST(test_gun_lookup_ultralyte);
    RUN_TEST(test_gun_lookup_ultralyte_observe);
    RUN_TEST(test_gun_lookup_pl2_observe);
    RUN_TEST(test_gun_lookup_pl3_observe);
    RUN_TEST(test_gun_lookup_atlanta_stealth_observe);
    RUN_TEST(test_gun_lookup_dragoneye_observe);
    RUN_TEST(test_gun_lookup_laser_ally_observe);
    RUN_TEST(test_gun_lookup_stalker_observe);
    RUN_TEST(test_gun_lookup_truspeed_observe);
    RUN_TEST(test_gun_lookup_observe_unknown);
    RUN_TEST(test_gun_lookup_stalker);
    RUN_TEST(test_gun_lookup_laser_ally);
    RUN_TEST(test_gun_lookup_atlanta_stealth);
    RUN_TEST(test_gun_lookup_unknown);
    RUN_TEST(test_gun_guncode_collision_d6_resolved_by_byte0);
    RUN_TEST(test_gun_guncode_collision_eb_resolved_by_byte0);

    // Alert burst parsing (all 8 guns + observe mode variants)
    RUN_TEST(test_burst_identifies_pl3);
    RUN_TEST(test_burst_identifies_dragoneye);
    RUN_TEST(test_burst_identifies_truspeed);
    RUN_TEST(test_burst_identifies_ultralyte);
    RUN_TEST(test_burst_identifies_ultralyte_observe);
    RUN_TEST(test_burst_identifies_pl2_observe);
    RUN_TEST(test_burst_identifies_pl3_observe);
    RUN_TEST(test_burst_identifies_atlanta_stealth_observe);
    RUN_TEST(test_burst_identifies_dragoneye_observe);
    RUN_TEST(test_burst_identifies_laser_ally_observe);
    RUN_TEST(test_burst_identifies_stalker_observe);
    RUN_TEST(test_burst_identifies_truspeed_observe);
    RUN_TEST(test_burst_identifies_stalker);
    RUN_TEST(test_burst_identifies_atlanta);
    RUN_TEST(test_burst_identifies_laser_ally);
    RUN_TEST(test_burst_identifies_pl2);
    RUN_TEST(test_burst_unknown_gun_still_alerts);

    // Heartbeat parsing
    RUN_TEST(test_heartbeat_transitions_idle_to_listening);
    RUN_TEST(test_paired_heartbeat_counted);
    RUN_TEST(test_discovery_poll_transitions_to_listening);

    // Timeouts
    RUN_TEST(test_heartbeat_timeout_returns_to_idle);
    RUN_TEST(test_heartbeat_keeps_listening_if_within_timeout);
    RUN_TEST(test_teardown_timeout_returns_to_listening);

    // Register write teardown trigger
    RUN_TEST(test_fd_terminator_triggers_teardown_from_alert);
    RUN_TEST(test_fd_terminator_d3_triggers_teardown);

    // Checksum-based resync
    RUN_TEST(test_resync_discards_garbage_before_heartbeat);
    RUN_TEST(test_resync_discards_garbage_before_alert_burst);
    RUN_TEST(test_bad_checksum_frame_rejected);

    // Noise window
    RUN_TEST(test_consecutive_bad_checksums_trigger_noise_window);
    RUN_TEST(test_noise_window_ends_on_valid_frame);

    // Snapshot
    RUN_TEST(test_snapshot_reflects_state);
    RUN_TEST(test_snapshot_default_values);

    // Alert status
    RUN_TEST(test_is_alert_active_during_alert);
    RUN_TEST(test_is_not_alert_active_during_listening);

    // String helpers
    RUN_TEST(test_gun_names_not_null);
    RUN_TEST(test_state_names_not_null);

    // Multi-burst
    RUN_TEST(test_sequential_bursts_update_gun);

    // Full lifecycle
    RUN_TEST(test_full_alert_lifecycle);

    // Standalone frames
    RUN_TEST(test_alert_trigger_standalone);

    // Observe-mode alert (98 02 00)
    RUN_TEST(test_observe_mode_alert_98_02_00);
    RUN_TEST(test_observe_mode_rearm_increments_burst_count);
    RUN_TEST(test_status_frame_98_other);

    // Heartbeat byte1 alert detection
    RUN_TEST(test_heartbeat_byte1_alert_transitions_to_alert_active);
    RUN_TEST(test_heartbeat_byte1_idle_resolves_alert);
    RUN_TEST(test_heartbeat_byte1_b8_does_not_trigger_alert);
    RUN_TEST(test_heartbeat_alert_repeated_01_no_double_trigger);
    RUN_TEST(test_heartbeat_alert_then_noise_from_listening);
    RUN_TEST(test_noise_from_listening_with_hb_alert);
    RUN_TEST(test_snapshot_includes_hb_byte1);
    RUN_TEST(test_teardown_clears_alert_flag);

    // Gun cleared on new alert entry (stale gun fix, Rev 6)
    RUN_TEST(test_new_alert_clears_stale_gun_via_98_trigger);
    RUN_TEST(test_new_alert_clears_stale_gun_via_heartbeat);
    RUN_TEST(test_new_alert_identifies_fresh_gun_after_clear);
    // Gun persists across in-engagement teardown↔alert cycling (alp_18 fix)
    RUN_TEST(test_gun_persists_through_teardown_rearm_cycle);

    // AlertSession / V1-shape display projection
    RUN_TEST(test_session_closed_by_default);
    // ownsLaserDisplay() — V1 laser suppression gate
    RUN_TEST(test_owns_laser_display_false_when_disabled);
    RUN_TEST(test_owns_laser_display_true_when_listening);
    RUN_TEST(test_owns_laser_display_true_during_alert);
    RUN_TEST(test_owns_laser_display_false_when_idle_after_timeout);
    RUN_TEST(test_session_opens_on_fresh_alert_from_listening);
    RUN_TEST(test_session_survives_teardown_rearm_cycle);
    RUN_TEST(test_session_closes_on_teardown_to_listening);
    RUN_TEST(test_event_gun_unknown_between_engagements_even_with_stale_lastgun);

    // Self-test suppression (bug #1)
    RUN_TEST(test_self_test_flagged_when_preamble_in_window);
    RUN_TEST(test_self_test_unflagged_when_real_gun_identified);
    RUN_TEST(test_self_test_not_flagged_without_preamble);
    RUN_TEST(test_self_test_preamble_after_window_ignored);
    RUN_TEST(test_self_test_envelope_expires_after_35s);

    // ALP SD logger CSV format
    RUN_TEST(test_sd_logger_frame_columns_match_header);
    RUN_TEST(test_sd_logger_heartbeat_includes_checksum);
    RUN_TEST(test_sd_logger_gun_id_jam_preserves_raw_frame);
    RUN_TEST(test_sd_logger_gun_id_observe_preserves_raw_frame);

    return UNITY_END();
}
