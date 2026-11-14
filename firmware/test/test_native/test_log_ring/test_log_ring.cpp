// Native tests for LogRing: the restart-surviving text log.

#include <unity.h>

#include <string.h>

#include <string>

#include "log_ring.h"

void setUp() {}
void tearDown() {}

namespace {
LogRingStore store;

std::string contents(const LogRingStore& s) {
    const char *a, *b;
    size_t al, bl;
    logRingSpans(s, a, al, b, bl);
    return std::string(a, al) + std::string(b, bl);
}
void put(const char* t) { logRingAppend(store, t, strlen(t)); }
}  // namespace

static void test_garbage_memory_starts_empty() {
    memset(&store, 0xA5, sizeof store);
    TEST_ASSERT_FALSE(logRingAttach(store));
    TEST_ASSERT_EQUAL_STRING("", contents(store).c_str());
}

static void test_log_survives_reattach() {
    memset(&store, 0, sizeof store);
    logRingAttach(store);
    put("boot 1\n");
    put("page timeout\n");
    TEST_ASSERT_TRUE(logRingAttach(store));  // as after ESP.restart()
    TEST_ASSERT_EQUAL_STRING("boot 1\npage timeout\n", contents(store).c_str());
}

static void test_corrupt_bookkeeping_resets() {
    logRingAttach(store);
    put("hello");
    store.head = kLogRingBytes + 7;
    TEST_ASSERT_FALSE(logRingAttach(store));
    TEST_ASSERT_EQUAL_STRING("", contents(store).c_str());
    put("x");
    store.used = kLogRingBytes + 1;
    TEST_ASSERT_FALSE(logRingAttach(store));
}

static void test_wraps_keeping_newest_bytes_in_order() {
    logRingClear(store);
    std::string expect;
    for (int i = 0; i < 1000; ++i) {
        char line[16];
        snprintf(line, sizeof line, "line %04d\n", i);  // 10 bytes
        put(line);
        expect += line;
    }
    const std::string got = contents(store);
    TEST_ASSERT_EQUAL(kLogRingBytes, got.size());
    TEST_ASSERT_TRUE(got == expect.substr(expect.size() - kLogRingBytes));
}

static void test_append_larger_than_ring_keeps_tail() {
    logRingClear(store);
    std::string big(kLogRingBytes * 2 + 3, 'a');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    logRingAppend(store, big.data(), big.size());
    TEST_ASSERT_TRUE(contents(store) == big.substr(big.size() - kLogRingBytes));
}

static void test_clear_empties() {
    put("something");
    logRingClear(store);
    TEST_ASSERT_EQUAL_STRING("", contents(store).c_str());
    TEST_ASSERT_TRUE(logRingAttach(store));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_garbage_memory_starts_empty);
    RUN_TEST(test_log_survives_reattach);
    RUN_TEST(test_corrupt_bookkeeping_resets);
    RUN_TEST(test_wraps_keeping_newest_bytes_in_order);
    RUN_TEST(test_append_larger_than_ring_keeps_tail);
    RUN_TEST(test_clear_empties);
    return UNITY_END();
}
