#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <Arduino.h>
#include "client_write_retry.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

class FakeClient {
public:
    explicit FakeClient(std::vector<size_t> scriptedWrites = {})
        : scriptedWrites_(std::move(scriptedWrites)) {}

    size_t write(const uint8_t* data, size_t size) {
        attempts_++;
        if (!scriptedWrites_.empty()) {
            const size_t scripted = scriptedWrites_.front();
            scriptedWrites_.erase(scriptedWrites_.begin());
            if (scripted == 0) {
                return 0;
            }
            const size_t accepted = std::min(scripted, size);
            written_.append(reinterpret_cast<const char*>(data), accepted);
            return accepted;
        }

        written_.append(reinterpret_cast<const char*>(data), size);
        return size;
    }

    int attempts() const { return attempts_; }
    const std::string& written() const { return written_; }

private:
    std::vector<size_t> scriptedWrites_;
    std::string written_;
    int attempts_ = 0;
};

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
}

void tearDown() {}

void test_write_client_bytes_retries_zero_writes() {
    FakeClient client({0, 0, 4});
    const uint8_t payload[] = {'T', 'E', 'S', 'T'};

    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload), 4);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(3, client.attempts());
    TEST_ASSERT_EQUAL_STRING_LEN("TEST", client.written().c_str(), 4);
}

void test_write_client_bytes_handles_partial_writes() {
    FakeClient client({2, 2, 1});
    const uint8_t payload[] = {'A', 'L', 'P', 'R', '!'};

    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload), 2);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(3, client.attempts());
    TEST_ASSERT_EQUAL_STRING_LEN("ALPR!", client.written().c_str(), 5);
}

void test_write_client_bytes_fails_after_retry_budget() {
    FakeClient client({0, 0, 0});
    const uint8_t payload[] = {'B', 'U', 'S'};

    const bool ok = client_write_retry::writeAll(client, payload, sizeof(payload), 2);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_INT(3, client.attempts());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(client.written().size()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_write_client_bytes_retries_zero_writes);
    RUN_TEST(test_write_client_bytes_handles_partial_writes);
    RUN_TEST(test_write_client_bytes_fails_after_retry_budget);
    return UNITY_END();
}
