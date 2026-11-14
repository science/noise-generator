// Native unit tests for schedule: is a local time inside the autoplay window?
//
// The window is given as minutes since local midnight. 21:00-09:00 crosses
// midnight. The start is inclusive and the end exclusive, so 21:00:00 is
// "in" and 09:00:00 is "out". Daylight saving is handled before this point
// (glue converts epoch -> local time with the POSIX TZ rule), so this logic
// only ever sees wall-clock minutes.

#include <unity.h>

#include "schedule.h"

void setUp() {}
void tearDown() {}

namespace {
constexpr int hm(int h, int m) { return h * 60 + m; }
const Window kNight{hm(21, 0), hm(9, 0)};
}  // namespace

static void test_night_window_crosses_midnight() {
    TEST_ASSERT_TRUE(inWindow(kNight, hm(23, 30)));
    TEST_ASSERT_TRUE(inWindow(kNight, hm(0, 0)));
    TEST_ASSERT_TRUE(inWindow(kNight, hm(3, 0)));
    TEST_ASSERT_TRUE(inWindow(kNight, hm(8, 59)));
}

static void test_daytime_is_outside() {
    TEST_ASSERT_FALSE(inWindow(kNight, hm(9, 30)));
    TEST_ASSERT_FALSE(inWindow(kNight, hm(12, 0)));
    TEST_ASSERT_FALSE(inWindow(kNight, hm(20, 59)));
}

static void test_start_inclusive_end_exclusive() {
    TEST_ASSERT_TRUE(inWindow(kNight, hm(21, 0)));
    TEST_ASSERT_FALSE(inWindow(kNight, hm(9, 0)));
}

static void test_same_day_window() {
    const Window nap{hm(13, 0), hm(15, 0)};
    TEST_ASSERT_FALSE(inWindow(nap, hm(12, 59)));
    TEST_ASSERT_TRUE(inWindow(nap, hm(13, 0)));
    TEST_ASSERT_TRUE(inWindow(nap, hm(14, 59)));
    TEST_ASSERT_FALSE(inWindow(nap, hm(15, 0)));
}

static void test_empty_window_is_never_in() {
    const Window none{hm(21, 0), hm(21, 0)};
    TEST_ASSERT_FALSE(inWindow(none, hm(21, 0)));
    TEST_ASSERT_FALSE(inWindow(none, hm(3, 0)));
}

static void test_minute_of_day_is_normalised() {
    TEST_ASSERT_TRUE(inWindow(kNight, hm(24, 30)));   // 00:30
    TEST_ASSERT_FALSE(inWindow(kNight, -hm(12, 0)));  // -12:00 == 12:00
}

static void test_pacific_tz_rule_string() {
    // POSIX TZ for US/Pacific: PST (UTC-8), PDT (UTC-7) from the 2nd Sunday
    // of March to the 1st Sunday of November, switching at 02:00.
    TEST_ASSERT_EQUAL_STRING("PST8PDT,M3.2.0,M11.1.0", kPacificTz);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_night_window_crosses_midnight);
    RUN_TEST(test_daytime_is_outside);
    RUN_TEST(test_start_inclusive_end_exclusive);
    RUN_TEST(test_same_day_window);
    RUN_TEST(test_empty_window_is_never_in);
    RUN_TEST(test_minute_of_day_is_normalised);
    RUN_TEST(test_pacific_tz_rule_string);
    return UNITY_END();
}
