// Native unit tests for Controller — the play/stop state machine.
//
// Rules (user, plan Q3, 2026-11-13), autoplay window 21:00-09:00:
//   1. Inside the window: plays automatically.
//      1.1 KEY1 while playing -> stop until the next window start.
//      1.2 KEY1 while stopped -> resume, playing until the window ends.
//   2. Outside the window:
//      2.1 KEY1 while playing -> stop (until the next window start).
//      2.2 KEY1 while stopped -> play for 2 hours.
//   3. A 2-hour run that reaches the window start hands over to the window:
//      it keeps playing until the window ends. The window has precedence.
// Plus: if the clock is unknown at boot, assume night (fail toward noise);
// the real window state is applied once the clock syncs.

#include <unity.h>

#include "controller.h"

void setUp() {}
void tearDown() {}

namespace {
constexpr uint32_t kMin = 60UL * 1000UL;
constexpr uint32_t kHour = 60UL * kMin;
}  // namespace

// --- rule 1: inside the window ------------------------------------------

static void test_plays_when_booted_inside_window() {
    Controller c(/*inWindow=*/true, 0);
    TEST_ASSERT_TRUE(c.shouldPlay());
}

static void test_silent_when_booted_outside_window() {
    Controller c(false, 0);
    TEST_ASSERT_FALSE(c.shouldPlay());
}

static void test_window_start_starts_playing() {
    Controller c(false, 0);
    c.setInWindow(true, 1000);
    TEST_ASSERT_TRUE(c.shouldPlay());
}

static void test_window_end_stops_playing() {
    Controller c(true, 0);
    c.setInWindow(false, 1000);
    TEST_ASSERT_FALSE(c.shouldPlay());
}

static void test_key_in_window_stops_until_next_window_start() {
    Controller c(true, 0);
    c.pressStartStop(1 * kHour);
    TEST_ASSERT_FALSE(c.shouldPlay());
    c.tick(5 * kHour);  // still night, still stopped
    TEST_ASSERT_FALSE(c.shouldPlay());
    c.setInWindow(false, 12 * kHour);  // 09:00
    TEST_ASSERT_FALSE(c.shouldPlay());
    c.setInWindow(true, 24 * kHour);  // next 21:00
    TEST_ASSERT_TRUE(c.shouldPlay());
}

static void test_key_again_in_window_resumes_until_window_end() {
    Controller c(true, 0);
    c.pressStartStop(1 * kHour);
    c.pressStartStop(2 * kHour);
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.tick(2 * kHour + 3 * kHour);  // no 2-hour limit inside the window
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.setInWindow(false, 12 * kHour);
    TEST_ASSERT_FALSE(c.shouldPlay());
}

static void test_resume_at_0850_ends_at_0900() {
    // F5 (user): START at 08:50 inside the window stops at 09:00.
    Controller c(true, 0);
    c.pressStartStop(1 * kHour);  // stopped in the night
    c.pressStartStop(11 * kHour + 50 * kMin);  // 08:50 resume
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.setInWindow(false, 12 * kHour);  // 09:00
    TEST_ASSERT_FALSE(c.shouldPlay());
}

// --- rule 2: outside the window ------------------------------------------

static void test_key_outside_window_plays_for_two_hours() {
    Controller c(false, 0);
    c.pressStartStop(10 * kMin);
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.tick(10 * kMin + 2 * kHour - 1);
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.tick(10 * kMin + 2 * kHour);
    TEST_ASSERT_FALSE(c.shouldPlay());
}

static void test_key_during_daytime_run_stops_it() {
    Controller c(false, 0);
    c.pressStartStop(0);
    c.pressStartStop(30 * kMin);
    TEST_ASSERT_FALSE(c.shouldPlay());
    c.tick(3 * kHour);
    TEST_ASSERT_FALSE(c.shouldPlay());
}

static void test_daytime_stop_still_lets_the_window_start() {
    Controller c(false, 0);
    c.pressStartStop(0);
    c.pressStartStop(30 * kMin);
    c.setInWindow(true, 5 * kHour);
    TEST_ASSERT_TRUE(c.shouldPlay());
}

static void test_restarting_a_daytime_run_gives_a_fresh_two_hours() {
    Controller c(false, 0);
    c.pressStartStop(0);
    c.pressStartStop(1 * kHour);  // stop
    c.pressStartStop(1 * kHour + kMin);  // start again
    c.tick(1 * kHour + kMin + 2 * kHour - 1);
    TEST_ASSERT_TRUE(c.shouldPlay());
}

// --- rule 3: a daytime run that reaches the window ----------------------

static void test_daytime_run_hands_over_to_the_window() {
    Controller c(false, 0);
    c.pressStartStop(0);           // 19:30, say: 2 h would end at 21:30
    c.setInWindow(true, 90 * kMin);  // 21:00 arrives mid-run
    c.tick(3 * kHour);             // past the 2-hour mark
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.setInWindow(false, 14 * kHour);  // 09:00
    TEST_ASSERT_FALSE(c.shouldPlay());
}

// --- clock unknown -------------------------------------------------------

static void test_unknown_clock_boots_playing_then_follows_real_window() {
    Controller c(Controller::kAssumeNightUntilClockKnown, 0);
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.setInWindow(false, 30000);  // clock synced: it is daytime
    TEST_ASSERT_FALSE(c.shouldPlay());
}

// --- status / reasons ----------------------------------------------------

static void test_state_names_explain_why() {
    Controller c(false, 0);
    TEST_ASSERT_EQUAL_STRING("idle (outside window)", c.stateName());
    c.pressStartStop(0);
    TEST_ASSERT_EQUAL_STRING("daytime run (2 h)", c.stateName());
    c.setInWindow(true, kMin);
    TEST_ASSERT_EQUAL_STRING("night window", c.stateName());
    c.pressStartStop(2 * kMin);
    TEST_ASSERT_EQUAL_STRING("stopped until next window", c.stateName());
}

static void test_run_remaining_reports_minutes_left() {
    Controller c(false, 0);
    c.pressStartStop(0);
    TEST_ASSERT_EQUAL_UINT32(120, c.runMinutesLeft(0));
    TEST_ASSERT_EQUAL_UINT32(90, c.runMinutesLeft(30 * kMin));
    Controller d(true, 0);
    TEST_ASSERT_EQUAL_UINT32(0, d.runMinutesLeft(0));
}

static void test_millis_wraparound_does_not_end_a_run_early() {
    // millis() wraps every ~49.7 days; a run started just before the wrap
    // must still last 2 hours.
    const uint32_t start = 0xFFFFFFFFUL - 10 * kMin;
    Controller c(false, start);
    c.pressStartStop(start);
    c.tick(start + 1 * kHour);  // wraps past zero
    TEST_ASSERT_TRUE(c.shouldPlay());
    c.tick(start + 2 * kHour);
    TEST_ASSERT_FALSE(c.shouldPlay());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_plays_when_booted_inside_window);
    RUN_TEST(test_silent_when_booted_outside_window);
    RUN_TEST(test_window_start_starts_playing);
    RUN_TEST(test_window_end_stops_playing);
    RUN_TEST(test_key_in_window_stops_until_next_window_start);
    RUN_TEST(test_key_again_in_window_resumes_until_window_end);
    RUN_TEST(test_resume_at_0850_ends_at_0900);
    RUN_TEST(test_key_outside_window_plays_for_two_hours);
    RUN_TEST(test_key_during_daytime_run_stops_it);
    RUN_TEST(test_daytime_stop_still_lets_the_window_start);
    RUN_TEST(test_restarting_a_daytime_run_gives_a_fresh_two_hours);
    RUN_TEST(test_daytime_run_hands_over_to_the_window);
    RUN_TEST(test_unknown_clock_boots_playing_then_follows_real_window);
    RUN_TEST(test_state_names_explain_why);
    RUN_TEST(test_run_remaining_reports_minutes_left);
    RUN_TEST(test_millis_wraparound_does_not_end_a_run_early);
    return UNITY_END();
}
