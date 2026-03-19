#include <unity.h>

#include "../../src/modules/lockout/lockout_band_policy.cpp"

void setUp() {
    lockoutSetKaLearningEnabled(false);
    lockoutSetKLearningEnabled(false);
    lockoutSetXLearningEnabled(false);
}

void tearDown() {
    lockoutSetKaLearningEnabled(false);
    lockoutSetKLearningEnabled(false);
    lockoutSetXLearningEnabled(false);
}

void test_enable_and_disable_preserves_other_band_bits() {
    TEST_ASSERT_EQUAL_UINT8(0u, lockoutSupportedBandMask());

    lockoutSetKaLearningEnabled(true);
    TEST_ASSERT_EQUAL_UINT8(0x02u, lockoutSupportedBandMask());

    lockoutSetKLearningEnabled(true);
    TEST_ASSERT_EQUAL_UINT8(0x06u, lockoutSupportedBandMask());

    lockoutSetXLearningEnabled(true);
    TEST_ASSERT_EQUAL_UINT8(kLockoutBandMaskKaKx, lockoutSupportedBandMask());

    lockoutSetKaLearningEnabled(false);
    TEST_ASSERT_EQUAL_UINT8(kLockoutBandMaskKxOnly, lockoutSupportedBandMask());

    lockoutSetKLearningEnabled(false);
    TEST_ASSERT_EQUAL_UINT8(0x08u, lockoutSupportedBandMask());

    lockoutSetXLearningEnabled(false);
    TEST_ASSERT_EQUAL_UINT8(0u, lockoutSupportedBandMask());
}

void test_sanitize_and_supported_follow_updated_mask() {
    lockoutSetKLearningEnabled(true);
    lockoutSetXLearningEnabled(true);

    TEST_ASSERT_EQUAL_UINT8(kLockoutBandMaskKxOnly, lockoutSupportedBandMask());
    TEST_ASSERT_EQUAL_UINT8(0x04u, lockoutSanitizeBandMask(0x06));
    TEST_ASSERT_TRUE(lockoutBandSupported(0x04));
    TEST_ASSERT_FALSE(lockoutBandSupported(0x02));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_enable_and_disable_preserves_other_band_bits);
    RUN_TEST(test_sanitize_and_supported_follow_updated_mask);
    return UNITY_END();
}
