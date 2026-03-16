#include <unity.h>
#include <cstring>

#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/lockout_index.cpp"
#include "../../src/modules/lockout/lockout_api_zone_write_service.cpp"  // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

void LockoutStore::begin(LockoutIndex* index) {
    index_ = index;
}

void LockoutStore::toJson(JsonDocument& doc) const {
    doc.clear();
    doc["_type"] = LockoutStore::kTypeTag;
    doc["_version"] = LockoutStore::kVersion;
    doc["areas"].to<JsonArray>();
}

bool LockoutStore::fromJson(JsonDocument&) {
    return true;
}

void LockoutLearner::clearCandidates() {}

size_t LockoutLearner::activeCandidateCount() const {
    return 0;
}

namespace {

bool responseContains(const WebServer& server, const char* needle) {
    return std::strstr(server.lastBody.c_str(), needle) != nullptr;
}

JsonDocument parseResponseBody(const WebServer& server) {
    JsonDocument doc;
    deserializeJson(doc, server.lastBody.c_str());
    return doc;
}

LockoutEntry makeLearnedEntry() {
    LockoutEntry entry;
    entry.latE5 = 3510000;
    entry.lonE5 = -8080000;
    entry.radiusE5 = 135;
    entry.areaId = 7;
    entry.bandMask = 0x04;
    entry.freqMHz = 24100;
    entry.freqTolMHz = 10;
    entry.freqWindowMinMHz = 24100;
    entry.freqWindowMaxMHz = 24100;
    entry.confidence = 80;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.directionMode = LockoutEntry::DIRECTION_ALL;
    return entry;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    lockoutSetKaLearningEnabled(false);
    lockoutSetKLearningEnabled(true);
    lockoutSetXLearningEnabled(true);
}

void tearDown() {}

void test_create_manual_zone_succeeds_and_marks_dirty() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    server.setArg(
        "plain",
        "{\"latitude\":35.1,\"longitude\":-80.8,\"bandMask\":4,\"radiusE5\":135,"
        "\"frequencyMHz\":24100,\"frequencyToleranceMHz\":10,\"confidence\":100,"
        "\"directionMode\":\"all\",\"headingDeg\":null,\"headingToleranceDeg\":45}");

    LockoutApiService::handleZoneCreate(server, index, store);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(store.isDirty());
    TEST_ASSERT_EQUAL_UINT32(1, index.activeCount());
    const LockoutEntry* entry = index.at(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_TRUE(entry->isActive());
    TEST_ASSERT_TRUE(entry->isManual());
    TEST_ASSERT_FALSE(entry->isLearned());
    TEST_ASSERT_EQUAL_UINT16(24100, entry->freqMHz);
    TEST_ASSERT_EQUAL_UINT16(24100, entry->freqWindowMinMHz);
    TEST_ASSERT_EQUAL_UINT16(24100, entry->freqWindowMaxMHz);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"manual\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"slot\":0"));
}

void test_create_rejects_missing_frequency() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    server.setArg(
        "plain",
        "{\"latitude\":35.1,\"longitude\":-80.8,\"bandMask\":4,\"radiusE5\":135,"
        "\"frequencyToleranceMHz\":10,\"confidence\":100,"
        "\"directionMode\":\"all\",\"headingDeg\":null,\"headingToleranceDeg\":45}");

    LockoutApiService::handleZoneCreate(server, index, store);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"frequencyMHz is required\""));
    TEST_ASSERT_FALSE(store.isDirty());
    TEST_ASSERT_EQUAL_UINT32(0, index.activeCount());
}

void test_create_rejects_duplicate_zone() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    server.setArg(
        "plain",
        "{\"latitude\":35.1,\"longitude\":-80.8,\"bandMask\":4,\"radiusE5\":135,"
        "\"frequencyMHz\":24100,\"frequencyToleranceMHz\":10,\"confidence\":100,"
        "\"directionMode\":\"all\",\"headingDeg\":null,\"headingToleranceDeg\":45}");
    LockoutApiService::handleZoneCreate(server, index, store);

    WebServer duplicateServer(80);
    duplicateServer.setArg(
        "plain",
        "{\"latitude\":35.1,\"longitude\":-80.8,\"bandMask\":4,\"radiusE5\":135,"
        "\"frequencyMHz\":24100,\"frequencyToleranceMHz\":10,\"confidence\":90,"
        "\"directionMode\":\"all\",\"headingDeg\":null,\"headingToleranceDeg\":45}");

    LockoutApiService::handleZoneCreate(duplicateServer, index, store);

    TEST_ASSERT_EQUAL_INT(409, duplicateServer.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(duplicateServer, "\"matching lockout zone already exists\""));
    TEST_ASSERT_TRUE(responseContains(duplicateServer, "\"slot\":0"));
    TEST_ASSERT_EQUAL_UINT32(1, index.activeCount());
}

void test_update_requires_slot() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    server.setArg("plain", "{\"latitude\":35.1,\"longitude\":-80.8,\"frequencyMHz\":24100}");

    LockoutApiService::handleZoneUpdate(server, index, store);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"slot is required\""));
}

void test_update_mutates_existing_zone_and_preserves_flags() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    const int slot = index.add(makeLearnedEntry());
    TEST_ASSERT_EQUAL_INT(0, slot);
    server.setArg(
        "plain",
        "{\"slot\":0,\"latitude\":35.2,\"longitude\":-80.7,\"bandMask\":4,\"radiusE5\":200,"
        "\"frequencyMHz\":24200,\"frequencyToleranceMHz\":12,\"confidence\":90,"
        "\"directionMode\":\"forward\",\"headingDeg\":180,\"headingToleranceDeg\":20}");

    LockoutApiService::handleZoneUpdate(server, index, store);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(store.isDirty());
    const LockoutEntry* entry = index.at(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_TRUE(entry->isActive());
    TEST_ASSERT_FALSE(entry->isManual());
    TEST_ASSERT_TRUE(entry->isLearned());
    TEST_ASSERT_EQUAL_UINT16(7, entry->areaId);
    TEST_ASSERT_EQUAL_INT32(3520000, entry->latE5);
    TEST_ASSERT_EQUAL_INT32(-8070000, entry->lonE5);
    TEST_ASSERT_EQUAL_UINT16(200, entry->radiusE5);
    TEST_ASSERT_EQUAL_UINT16(24200, entry->freqMHz);
    TEST_ASSERT_EQUAL_UINT16(24200, entry->freqWindowMinMHz);
    TEST_ASSERT_EQUAL_UINT16(24200, entry->freqWindowMaxMHz);
    TEST_ASSERT_EQUAL_UINT8(LockoutEntry::DIRECTION_FORWARD, entry->directionMode);
    TEST_ASSERT_EQUAL_UINT16(180, entry->headingDeg);
    TEST_ASSERT_EQUAL_UINT8(20, entry->headingTolDeg);
    TEST_ASSERT_TRUE(responseContains(server, "\"success\":true"));
    TEST_ASSERT_TRUE(responseContains(server, "\"learned\":true"));
}

void test_update_rejects_duplicate_zone_signature() {
    WebServer server(80);
    LockoutIndex index;
    LockoutStore store;
    LockoutEntry first = makeLearnedEntry();
    LockoutEntry second = makeLearnedEntry();
    second.latE5 = 3520000;
    second.lonE5 = -8070000;
    second.areaId = 8;
    second.freqMHz = 24200;
    second.freqWindowMinMHz = 24200;
    second.freqWindowMaxMHz = 24200;
    TEST_ASSERT_EQUAL_INT(0, index.add(first));
    TEST_ASSERT_EQUAL_INT(1, index.add(second));

    server.setArg(
        "plain",
        "{\"slot\":1,\"latitude\":35.1,\"longitude\":-80.8,\"bandMask\":4,\"radiusE5\":135,"
        "\"frequencyMHz\":24100,\"frequencyToleranceMHz\":10,\"confidence\":90,"
        "\"directionMode\":\"all\",\"headingDeg\":null,\"headingToleranceDeg\":45}");

    LockoutApiService::handleZoneUpdate(server, index, store);

    TEST_ASSERT_EQUAL_INT(409, server.lastStatusCode);
    TEST_ASSERT_TRUE(responseContains(server, "\"matching lockout zone already exists\""));
    TEST_ASSERT_TRUE(responseContains(server, "\"slot\":0"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_create_manual_zone_succeeds_and_marks_dirty);
    RUN_TEST(test_create_rejects_missing_frequency);
    RUN_TEST(test_create_rejects_duplicate_zone);
    RUN_TEST(test_update_requires_slot);
    RUN_TEST(test_update_mutates_existing_zone_and_preserves_flags);
    RUN_TEST(test_update_rejects_duplicate_zone_signature);
    return UNITY_END();
}
