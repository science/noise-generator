// Native unit tests for board_id: deciding which AI-Thinker ESP32-A1S Audio
// Kit variant we are on from what an I2C scan finds on each candidate bus.
//
// Why this matters: the two ES8388 variants wire the codec's I2C bus to
// different pins (v1: SDA 33 / SCL 32; v2: SDA 18 / SCL 23). On v2, GPIO 23,
// 18 and 5 belong to the codec, so KEY4/KEY5/KEY6 are unusable. Getting the
// variant wrong means a silent codec or buttons that read garbage.

#include <unity.h>
#include "board_id.h"

void setUp() {}
void tearDown() {}

static void test_es8388_on_v1_bus_is_v1() {
    uint8_t busV1[] = {0x10};
    TEST_ASSERT_EQUAL(BoardVariant::Es8388V1, classifyBoard(busV1, 1, nullptr, 0));
}

static void test_es8388_on_v2_bus_is_v2() {
    uint8_t busV2[] = {0x10};
    TEST_ASSERT_EQUAL(BoardVariant::Es8388V2, classifyBoard(nullptr, 0, busV2, 1));
}

static void test_ac101_on_v1_bus_is_ac101() {
    uint8_t busV1[] = {0x1A};
    TEST_ASSERT_EQUAL(BoardVariant::Ac101, classifyBoard(busV1, 1, nullptr, 0));
}

static void test_nothing_found_is_unknown() {
    TEST_ASSERT_EQUAL(BoardVariant::Unknown, classifyBoard(nullptr, 0, nullptr, 0));
}

static void test_unrelated_devices_are_unknown() {
    uint8_t busV1[] = {0x3C, 0x68};
    TEST_ASSERT_EQUAL(BoardVariant::Unknown, classifyBoard(busV1, 2, nullptr, 0));
}

static void test_codec_on_both_buses_is_ambiguous() {
    // Should never happen on real hardware; refuse to guess.
    uint8_t a[] = {0x10};
    uint8_t b[] = {0x10};
    TEST_ASSERT_EQUAL(BoardVariant::Ambiguous, classifyBoard(a, 1, b, 1));
}

static void test_es8388_found_among_other_devices() {
    uint8_t busV1[] = {0x08, 0x10, 0x50};
    TEST_ASSERT_EQUAL(BoardVariant::Es8388V1, classifyBoard(busV1, 3, nullptr, 0));
}

static void test_usable_keys_v1_with_sd_card() {
    // KEY2 (GPIO13) is the SD card's DAT3 line, so it is lost when SD is used.
    TEST_ASSERT_EQUAL_STRING("1,3,4,5,6", usableKeys(BoardVariant::Es8388V1, true));
}

static void test_usable_keys_v1_without_sd_card() {
    TEST_ASSERT_EQUAL_STRING("1,2,3,4,5,6", usableKeys(BoardVariant::Es8388V1, false));
}

static void test_usable_keys_v2_with_sd_card() {
    // v2 also loses KEY4/5/6 to the codec's I2C (23, 18) and I2S BCK (5).
    TEST_ASSERT_EQUAL_STRING("1,3", usableKeys(BoardVariant::Es8388V2, true));
}

static void test_usable_keys_v2_without_sd_card() {
    TEST_ASSERT_EQUAL_STRING("1,2,3", usableKeys(BoardVariant::Es8388V2, false));
}

static void test_usable_keys_unknown_is_empty() {
    TEST_ASSERT_EQUAL_STRING("", usableKeys(BoardVariant::Unknown, true));
}

static void test_variant_names() {
    TEST_ASSERT_EQUAL_STRING("ES8388 v1 (I2C 33/32)", variantName(BoardVariant::Es8388V1));
    TEST_ASSERT_EQUAL_STRING("ES8388 v2 (I2C 18/23)", variantName(BoardVariant::Es8388V2));
    TEST_ASSERT_EQUAL_STRING("AC101 (end-of-life codec)", variantName(BoardVariant::Ac101));
    TEST_ASSERT_EQUAL_STRING("unknown", variantName(BoardVariant::Unknown));
    TEST_ASSERT_EQUAL_STRING("ambiguous", variantName(BoardVariant::Ambiguous));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_es8388_on_v1_bus_is_v1);
    RUN_TEST(test_es8388_on_v2_bus_is_v2);
    RUN_TEST(test_ac101_on_v1_bus_is_ac101);
    RUN_TEST(test_nothing_found_is_unknown);
    RUN_TEST(test_unrelated_devices_are_unknown);
    RUN_TEST(test_codec_on_both_buses_is_ambiguous);
    RUN_TEST(test_es8388_found_among_other_devices);
    RUN_TEST(test_usable_keys_v1_with_sd_card);
    RUN_TEST(test_usable_keys_v1_without_sd_card);
    RUN_TEST(test_usable_keys_v2_with_sd_card);
    RUN_TEST(test_usable_keys_v2_without_sd_card);
    RUN_TEST(test_usable_keys_unknown_is_empty);
    RUN_TEST(test_variant_names);
    return UNITY_END();
}
