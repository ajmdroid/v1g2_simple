#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/packet_parser.h"
#include "../mocks/display.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/display/display_preview_module.cpp"

static V1Display display;
static DisplayPreviewModule module;

void setUp() {
    display.reset();
    module = DisplayPreviewModule{};
    module.begin(&display);
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_preview_sequence_includes_camera_steps() {
    module.requestHold(5500);

    mockMillis = 1000;  // 0 ms
    module.update();
    mockMillis = 2000;  // 1000 ms
    module.update();
    mockMillis = 3000;  // 2000 ms
    module.update();
    mockMillis = 4000;  // 3000 ms
    module.update();
    mockMillis = 5000;  // 4000 ms
    module.update();

    TEST_ASSERT_EQUAL_INT(5, display.updateCalls);
    TEST_ASSERT_EQUAL_INT(0, display.updateCameraAlertCalls);

    mockMillis = 5500;  // 4500 ms
    module.update();
    TEST_ASSERT_EQUAL_INT(5, display.updateCalls);
    TEST_ASSERT_EQUAL_INT(1, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CameraType::SPEED),
                          static_cast<int>(display.lastCameraPayload.type));
    TEST_ASSERT_EQUAL_UINT16(30480, display.lastCameraPayload.distanceCm);

    mockMillis = 6000;  // 5000 ms
    module.update();
    TEST_ASSERT_EQUAL_INT(5, display.updateCalls);
    TEST_ASSERT_EQUAL_INT(2, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CameraType::RED_LIGHT),
                          static_cast<int>(display.lastCameraPayload.type));
    TEST_ASSERT_EQUAL_UINT16(15240, display.lastCameraPayload.distanceCm);
}

void test_preview_ends_after_requested_hold_with_tail() {
    module.requestHold(1000);

    mockMillis = 1000;  // 0 ms
    module.update();
    TEST_ASSERT_TRUE(module.isRunning());
    TEST_ASSERT_FALSE(module.consumeEnded());

    mockMillis = 2599;  // 1599 ms elapsed
    module.update();
    TEST_ASSERT_TRUE(module.isRunning());
    TEST_ASSERT_FALSE(module.consumeEnded());

    mockMillis = 2600;  // 1600 ms elapsed (1000 hold + 600 tail)
    module.update();
    TEST_ASSERT_FALSE(module.isRunning());
    TEST_ASSERT_TRUE(module.consumeEnded());
    TEST_ASSERT_FALSE(module.consumeEnded());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_preview_sequence_includes_camera_steps);
    RUN_TEST(test_preview_ends_after_requested_hold_with_tail);
    return UNITY_END();
}
