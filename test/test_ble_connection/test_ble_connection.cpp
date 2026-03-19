#include <unity.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
}

std::string extractFunctionBody(const std::string& text, const std::string& signature) {
    const size_t sigPos = text.find(signature);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sigPos);

    const size_t braceStart = text.find('{', sigPos);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, braceStart);

    int depth = 0;
    for (size_t i = braceStart; i < text.size(); ++i) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
            if (depth == 0) {
                return text.substr(braceStart, i - braceStart + 1);
            }
        }
    }

    TEST_FAIL_MESSAGE("Failed to locate function body end");
    return {};
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_async_connect_does_not_delete_bond() {
    const std::filesystem::path source =
        std::filesystem::path("/Users/ajmedford/v1g2_simple/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body = extractFunctionBody(text, "bool V1BLEClient::startAsyncConnect()");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("NimBLEDevice::deleteBond("));
}

void test_disconnect_callback_still_defers_bond_heal() {
    const std::filesystem::path source =
        std::filesystem::path("/Users/ajmedford/v1g2_simple/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDeleteBondAddr"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("pendingDeleteBond = true"));
}

void test_disconnect_callback_no_longer_stops_proxy_advertising_inline() {
    const std::filesystem::path source =
        std::filesystem::path("/Users/ajmedford/v1g2_simple/src/ble_connection.cpp");
    const std::string text = readFile(source);
    const std::string body =
        extractFunctionBody(text, "void V1BLEClient::ClientCallbacks::onDisconnect");

    TEST_ASSERT_EQUAL(std::string::npos, body.find("NimBLEDevice::stopAdvertising("));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, body.find("enqueueProxyCallbackEvent"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_async_connect_does_not_delete_bond);
    RUN_TEST(test_disconnect_callback_still_defers_bond_heal);
    RUN_TEST(test_disconnect_callback_no_longer_stops_proxy_advertising_inline);
    return UNITY_END();
}
