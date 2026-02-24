#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_visual_sync_module.cpp"

static WifiVisualSyncModule module;
static int drawCalls = 0;
static unsigned long lastDrawMs = 0;

static void resetDrawState() {
    drawCalls = 0;
    lastDrawMs = 0;
}

static std::function<void()> makeDrawCallback(unsigned long nowMs) {
    return [nowMs] {
        drawCalls++;
        lastDrawMs = nowMs;
    };
}

void setUp() {
    module.reset();
    resetDrawState();
}

void tearDown() {}

void test_state_transition_triggers_draw() {
    module.process(1000, false, false, false, makeDrawCallback(1000));
    TEST_ASSERT_EQUAL_INT(0, drawCalls);

    module.process(1100, true, false, false, makeDrawCallback(1100));
    TEST_ASSERT_EQUAL_INT(1, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(1100, lastDrawMs);
}

void test_periodic_refresh_runs_every_2s_when_active() {
    module.process(100, true, false, false, makeDrawCallback(100));
    TEST_ASSERT_EQUAL_INT(1, drawCalls);

    module.process(2099, true, false, false, makeDrawCallback(2099));
    TEST_ASSERT_EQUAL_INT(1, drawCalls);

    module.process(2100, true, false, false, makeDrawCallback(2100));
    TEST_ASSERT_EQUAL_INT(2, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(2100, lastDrawMs);
}

void test_preview_or_boot_hold_blocks_draw_but_preserves_state_machine() {
    module.process(1000, true, true, false, makeDrawCallback(1000));
    TEST_ASSERT_EQUAL_INT(0, drawCalls);

    module.process(1500, true, false, false, makeDrawCallback(1500));
    TEST_ASSERT_EQUAL_INT(0, drawCalls);

    module.process(2000, true, false, false, makeDrawCallback(2000));
    TEST_ASSERT_EQUAL_INT(1, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(2000, lastDrawMs);

    module.process(4000, true, false, true, makeDrawCallback(4000));
    TEST_ASSERT_EQUAL_INT(1, drawCalls);

    module.process(4001, true, false, false, makeDrawCallback(4001));
    TEST_ASSERT_EQUAL_INT(2, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(4001, lastDrawMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_state_transition_triggers_draw);
    RUN_TEST(test_periodic_refresh_runs_every_2s_when_active);
    RUN_TEST(test_preview_or_boot_hold_blocks_draw_but_preserves_state_machine);
    return UNITY_END();
}
