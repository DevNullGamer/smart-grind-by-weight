#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "system/screen_power.h"

static const uint32_t MIN1 = 60000u;

static ScreenPowerInput base_in(uint32_t dim, uint32_t off, uint32_t idle) {
    ScreenPowerInput in;
    in.ms_since_touch = idle;
    in.ms_since_weight_activity = idle;
    in.ms_since_strong_weight_activity = idle;
    in.dim_after_ms = dim;
    in.off_after_ms = off;
    in.keep_awake = false;
    in.keep_on = false;
    in.current = ScreenLevel::AWAKE;
    return in;
}

// ---- screen_power_decide ----
static void test_decide_both_never_always_awake() {
    uint32_t idles[] = {0, 1, MIN1, 100u * MIN1, UINT32_MAX};
    for (uint32_t idle : idles) {
        for (int c = 0; c < 3; c++) {
            ScreenPowerInput in = base_in(0, 0, idle);
            in.current = (ScreenLevel)c;
            TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
        }
    }
}

static void test_decide_dim_only() {
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(base_in(5 * MIN1, 0, 5 * MIN1 - 1)));
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(base_in(5 * MIN1, 0, 5 * MIN1)));
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(base_in(5 * MIN1, 0, UINT32_MAX)));
}

static void test_decide_off_only_goes_awake_to_off_directly() {
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(base_in(0, 10 * MIN1, 10 * MIN1 - 1)));
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(base_in(0, 10 * MIN1, 10 * MIN1)));
}

static void test_decide_dim_then_off_thresholds() {
    ScreenPowerInput in = base_in(MIN1, 10 * MIN1, MIN1 - 1);
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
    in.ms_since_touch = in.ms_since_weight_activity = in.ms_since_strong_weight_activity = MIN1;
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(in));
    in.ms_since_touch = in.ms_since_weight_activity = in.ms_since_strong_weight_activity = 10 * MIN1 - 1;
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(in));
    in.ms_since_touch = in.ms_since_weight_activity = in.ms_since_strong_weight_activity = 10 * MIN1;
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(in));
}

static void test_decide_off_not_after_dim_off_wins() {
    // off == dim and off < dim: OFF wins when due.
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(base_in(5 * MIN1, 5 * MIN1, 5 * MIN1)));
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(base_in(10 * MIN1, 5 * MIN1, 5 * MIN1)));
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(base_in(10 * MIN1, 5 * MIN1, 5 * MIN1 - 1)));
    // off < dim and idle between: nothing due until off.
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(base_in(10 * MIN1, 5 * MIN1, 7 * MIN1)));
}

static void test_decide_keep_awake_beats_everything() {
    for (int c = 0; c < 3; c++) {
        ScreenPowerInput in = base_in(MIN1, 2 * MIN1, UINT32_MAX);
        in.keep_awake = true;
        in.keep_on = true;
        in.current = (ScreenLevel)c;
        TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
        in.keep_on = false;
        TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
    }
}

static void test_decide_keep_on_blocks_off_only() {
    ScreenPowerInput in = base_in(MIN1, 2 * MIN1, 5 * MIN1);
    in.keep_on = true;
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(in));
    in.current = ScreenLevel::OFF;  // already OFF, keep_on set: must leave OFF
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(in));
    in.dim_after_ms = 0;
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
    in.keep_on = false;
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(in));
}

static void test_decide_idle_is_min_of_touch_and_weight() {
    ScreenPowerInput in = base_in(MIN1, 10 * MIN1, 20 * MIN1);
    in.ms_since_touch = 100;  // recent touch
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
    in.ms_since_touch = 20 * MIN1;
    in.ms_since_weight_activity = 100;  // recent weight
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
    // Both old: off.
    in.ms_since_weight_activity = 20 * MIN1;
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(in));
    // Weight old, touch moderate: dimmed (min = touch).
    in.ms_since_touch = 2 * MIN1;
    TEST_ASSERT_EQUAL(ScreenLevel::DIMMED, screen_power_decide(in));
}

static void test_decide_weak_weight_does_not_wake_from_off() {
    ScreenPowerInput in = base_in(MIN1, 10 * MIN1, 20 * MIN1);
    in.current = ScreenLevel::OFF;
    in.ms_since_weight_activity = 50;  // 1 g change just now
    in.ms_since_strong_weight_activity = 20 * MIN1;
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(in));
}

static void test_decide_strong_weight_wakes_from_off() {
    ScreenPowerInput in = base_in(MIN1, 10 * MIN1, 20 * MIN1);
    in.current = ScreenLevel::OFF;
    in.ms_since_strong_weight_activity = 50;
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
}

static void test_decide_weak_weight_counts_when_not_off() {
    for (int c = 0; c < 2; c++) {
        ScreenPowerInput in = base_in(MIN1, 10 * MIN1, 20 * MIN1);
        in.current = (ScreenLevel)c;  // AWAKE, DIMMED
        in.ms_since_weight_activity = 50;
        in.ms_since_strong_weight_activity = 20 * MIN1;
        TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
    }
}

static void test_decide_touch_wakes_from_off() {
    ScreenPowerInput in = base_in(MIN1, 10 * MIN1, 20 * MIN1);
    in.current = ScreenLevel::OFF;
    in.ms_since_touch = 0;
    TEST_ASSERT_EQUAL(ScreenLevel::AWAKE, screen_power_decide(in));
}

static void test_decide_off_stays_off_without_activity() {
    ScreenPowerInput in = base_in(MIN1, 10 * MIN1, 20 * MIN1);
    in.current = ScreenLevel::OFF;
    TEST_ASSERT_EQUAL(ScreenLevel::OFF, screen_power_decide(in));
}

// ---- screen_state_allows_off ----
static void test_state_allows_off_all_states() {
    TEST_ASSERT_TRUE(screen_state_allows_off(UIState::READY));
    TEST_ASSERT_TRUE(screen_state_allows_off(UIState::EDIT));
    TEST_ASSERT_TRUE(screen_state_allows_off(UIState::MENU));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::GRINDING));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::GRIND_COMPLETE));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::GRIND_TIMEOUT));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::CONFIRM));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::PURGE_CONFIRM));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::CALIBRATION));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::AUTOTUNING));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::OTA_UPDATE));
    TEST_ASSERT_FALSE(screen_state_allows_off(UIState::OTA_UPDATE_FAILED));
}

// ---- screen_error_blocks_off ----
static void test_error_blocks_off() {
    const uint32_t A = 1u << 3, B = 1u << 7, E = 1u << 1, F = 1u << 31;
    const uint32_t adv = A | B;
    TEST_ASSERT_FALSE(screen_error_blocks_off(0, adv));
    TEST_ASSERT_FALSE(screen_error_blocks_off(A, adv));
    TEST_ASSERT_FALSE(screen_error_blocks_off(B, adv));
    TEST_ASSERT_FALSE(screen_error_blocks_off(A | B, adv));
    TEST_ASSERT_TRUE(screen_error_blocks_off(A | E, adv));
    TEST_ASSERT_TRUE(screen_error_blocks_off(E, adv));
    TEST_ASSERT_TRUE(screen_error_blocks_off(F, adv));
    TEST_ASSERT_TRUE(screen_error_blocks_off(A | B | F, adv));
    // Empty advisory set: any bit blocks.
    TEST_ASSERT_TRUE(screen_error_blocks_off(A, 0));
    TEST_ASSERT_FALSE(screen_error_blocks_off(0, 0));
}

// ---- screen_input_block_next ----
static void test_block_off_always_blocked() {
    TEST_ASSERT_TRUE(screen_input_block_next(false, ScreenLevel::AWAKE, ScreenLevel::OFF, false));
    TEST_ASSERT_TRUE(screen_input_block_next(false, ScreenLevel::DIMMED, ScreenLevel::OFF, true));
    TEST_ASSERT_TRUE(screen_input_block_next(true, ScreenLevel::OFF, ScreenLevel::OFF, false));
    TEST_ASSERT_TRUE(screen_input_block_next(true, ScreenLevel::OFF, ScreenLevel::OFF, true));
}

static void test_block_wake_with_touch_held_then_release() {
    bool b = screen_input_block_next(true, ScreenLevel::OFF, ScreenLevel::OFF, true);
    TEST_ASSERT_TRUE(b);
    // Wake: OFF -> AWAKE with finger down.
    b = screen_input_block_next(b, ScreenLevel::OFF, ScreenLevel::AWAKE, true);
    TEST_ASSERT_TRUE(b);
    // Long continuous press: stays blocked on later ticks (prev no longer OFF).
    for (int i = 0; i < 50; i++) {
        b = screen_input_block_next(b, ScreenLevel::AWAKE, ScreenLevel::AWAKE, true);
        TEST_ASSERT_TRUE(b);
    }
    // Release clears.
    b = screen_input_block_next(b, ScreenLevel::AWAKE, ScreenLevel::AWAKE, false);
    TEST_ASSERT_FALSE(b);
    // Next touch is free.
    b = screen_input_block_next(b, ScreenLevel::AWAKE, ScreenLevel::AWAKE, true);
    TEST_ASSERT_FALSE(b);
}

static void test_block_wake_to_dimmed_also_swallows() {
    TEST_ASSERT_TRUE(screen_input_block_next(true, ScreenLevel::OFF, ScreenLevel::DIMMED, true));
    TEST_ASSERT_FALSE(screen_input_block_next(true, ScreenLevel::OFF, ScreenLevel::DIMMED, false));
}

static void test_block_wake_by_weight_no_touch_clears_at_once() {
    TEST_ASSERT_FALSE(screen_input_block_next(true, ScreenLevel::OFF, ScreenLevel::AWAKE, false));
}

static void test_block_never_blocks_when_not_off() {
    ScreenLevel lv[] = {ScreenLevel::AWAKE, ScreenLevel::DIMMED};
    for (ScreenLevel p : lv)
        for (ScreenLevel n : lv)
            for (int t = 0; t < 2; t++)
                TEST_ASSERT_FALSE(screen_input_block_next(false, p, n, t != 0));
}

// ---- storage normaliser ----
static void test_storage_normalise() {
    // dim default 5
    TEST_ASSERT_EQUAL(5, screen_minutes_normalise_storage(-1, kDimMinutesSteps, kDimMinutesStepCount, 5));
    TEST_ASSERT_EQUAL(5, screen_minutes_normalise_storage(3, kDimMinutesSteps, kDimMinutesStepCount, 5));
    TEST_ASSERT_EQUAL(5, screen_minutes_normalise_storage(INT32_MAX, kDimMinutesSteps, kDimMinutesStepCount, 5));
    TEST_ASSERT_EQUAL(5, screen_minutes_normalise_storage(INT32_MIN, kDimMinutesSteps, kDimMinutesStepCount, 5));
    TEST_ASSERT_EQUAL(0, screen_minutes_normalise_storage(0, kDimMinutesSteps, kDimMinutesStepCount, 5));
    for (int i = 0; i < kDimMinutesStepCount; i++)
        TEST_ASSERT_EQUAL(kDimMinutesSteps[i],
                          screen_minutes_normalise_storage(kDimMinutesSteps[i], kDimMinutesSteps, kDimMinutesStepCount, 5));
    // off default 60
    TEST_ASSERT_EQUAL(60, screen_minutes_normalise_storage(-10, kOffMinutesSteps, kOffMinutesStepCount, 60));
    TEST_ASSERT_EQUAL(60, screen_minutes_normalise_storage(5, kOffMinutesSteps, kOffMinutesStepCount, 60));
    TEST_ASSERT_EQUAL(60, screen_minutes_normalise_storage(481, kOffMinutesSteps, kOffMinutesStepCount, 60));
    TEST_ASSERT_EQUAL(0, screen_minutes_normalise_storage(0, kOffMinutesSteps, kOffMinutesStepCount, 60));
    TEST_ASSERT_EQUAL(480, screen_minutes_normalise_storage(480, kOffMinutesSteps, kOffMinutesStepCount, 60));
}

// ---- API normaliser ----
static int api_dim(double v) { return screen_minutes_normalise_api(v, kDimMinutesSteps, kDimMinutesStepCount); }
static int api_off(double v) { return screen_minutes_normalise_api(v, kOffMinutesSteps, kOffMinutesStepCount); }

static void test_api_never_cases() {
    TEST_ASSERT_EQUAL(0, api_dim(-5));
    TEST_ASSERT_EQUAL(0, api_dim(0));
    TEST_ASSERT_EQUAL(0, api_dim(-0.0));
    TEST_ASSERT_EQUAL(0, api_dim(0.4));
    TEST_ASSERT_EQUAL(0, api_dim(NAN));
    TEST_ASSERT_EQUAL(0, api_dim(-INFINITY));
    TEST_ASSERT_EQUAL(0, api_off(-5));
    TEST_ASSERT_EQUAL(0, api_off(0.4));
    TEST_ASSERT_EQUAL(0, api_off(NAN));
    TEST_ASSERT_EQUAL(0, api_off(-INFINITY));
}

static void test_api_nearest_and_ties() {
    TEST_ASSERT_EQUAL(1, api_dim(0.5));   // rounds to 1
    TEST_ASSERT_EQUAL(1, api_dim(1));
    TEST_ASSERT_EQUAL(2, api_dim(3));     // 2 is nearer than 5
    TEST_ASSERT_EQUAL(5, api_dim(4));     // 5 nearer
    TEST_ASSERT_EQUAL(60, api_dim(61));
    TEST_ASSERT_EQUAL(60, api_dim(59.6));
    TEST_ASSERT_EQUAL(30, api_dim(45));   // tie 30/60 -> lower
    TEST_ASSERT_EQUAL(60, api_dim(46));
    TEST_ASSERT_EQUAL(30, api_dim(44));
    TEST_ASSERT_EQUAL(10, api_off(20));   // tie 10/30 -> lower
    TEST_ASSERT_EQUAL(30, api_off(21));
    TEST_ASSERT_EQUAL(10, api_off(20.4)); // rounds to 20 -> tie
    TEST_ASSERT_EQUAL(10, api_off(3));    // below first step -> first step
    TEST_ASSERT_EQUAL(10, api_off(1));
    TEST_ASSERT_EQUAL(60, api_off(90));   // tie 60/120 -> lower
    TEST_ASSERT_EQUAL(120, api_off(180)); // tie 120/240 -> lower
    TEST_ASSERT_EQUAL(240, api_off(360)); // tie 240/480 -> lower
}

static void test_api_above_largest_is_largest() {
    TEST_ASSERT_EQUAL(60, api_dim(60));
    TEST_ASSERT_EQUAL(60, api_dim(99999));
    TEST_ASSERT_EQUAL(60, api_dim(1e300));
    TEST_ASSERT_EQUAL(60, api_dim(INFINITY));
    TEST_ASSERT_EQUAL(480, api_off(480));
    TEST_ASSERT_EQUAL(480, api_off(99999));
    TEST_ASSERT_EQUAL(480, api_off(INFINITY));
}

static void test_api_every_listed_step_maps_to_itself() {
    for (int i = 0; i < kDimMinutesStepCount - 1; i++) TEST_ASSERT_EQUAL(kDimMinutesSteps[i], api_dim(kDimMinutesSteps[i]));
    for (int i = 0; i < kOffMinutesStepCount - 1; i++) TEST_ASSERT_EQUAL(kOffMinutesSteps[i], api_off(kOffMinutesSteps[i]));
}

// ---- index helpers ----
static void test_steps_tables() {
    TEST_ASSERT_EQUAL(8, kDimMinutesStepCount);
    TEST_ASSERT_EQUAL(7, kOffMinutesStepCount);
    TEST_ASSERT_EQUAL(0, kDimMinutesSteps[kDimMinutesStepCount - 1]);
    TEST_ASSERT_EQUAL(0, kOffMinutesSteps[kOffMinutesStepCount - 1]);
    const int dim[] = {1, 2, 5, 10, 15, 30, 60, 0};
    const int off[] = {10, 30, 60, 120, 240, 480, 0};
    for (int i = 0; i < 8; i++) TEST_ASSERT_EQUAL(dim[i], kDimMinutesSteps[i]);
    for (int i = 0; i < 7; i++) TEST_ASSERT_EQUAL(off[i], kOffMinutesSteps[i]);
}

static void test_index_round_trip() {
    for (int i = 0; i < kDimMinutesStepCount; i++) {
        int m = screen_minutes_from_index(i, kDimMinutesSteps, kDimMinutesStepCount);
        TEST_ASSERT_EQUAL(kDimMinutesSteps[i], m);
        TEST_ASSERT_EQUAL(i, screen_minutes_to_index(m, kDimMinutesSteps, kDimMinutesStepCount));
    }
    for (int i = 0; i < kOffMinutesStepCount; i++) {
        int m = screen_minutes_from_index(i, kOffMinutesSteps, kOffMinutesStepCount);
        TEST_ASSERT_EQUAL(kOffMinutesSteps[i], m);
        TEST_ASSERT_EQUAL(i, screen_minutes_to_index(m, kOffMinutesSteps, kOffMinutesStepCount));
    }
}

static void test_index_clamped() {
    TEST_ASSERT_EQUAL(1, screen_minutes_from_index(-1, kDimMinutesSteps, kDimMinutesStepCount));
    TEST_ASSERT_EQUAL(1, screen_minutes_from_index(INT32_MIN, kDimMinutesSteps, kDimMinutesStepCount));
    TEST_ASSERT_EQUAL(0, screen_minutes_from_index(8, kDimMinutesSteps, kDimMinutesStepCount));
    TEST_ASSERT_EQUAL(0, screen_minutes_from_index(1000, kDimMinutesSteps, kDimMinutesStepCount));
    TEST_ASSERT_EQUAL(10, screen_minutes_from_index(-3, kOffMinutesSteps, kOffMinutesStepCount));
    TEST_ASSERT_EQUAL(0, screen_minutes_from_index(7, kOffMinutesSteps, kOffMinutesStepCount));
}

static void test_to_index_unlisted_maps_to_nearest() {
    TEST_ASSERT_EQUAL(1, screen_minutes_to_index(3, kDimMinutesSteps, kDimMinutesStepCount));   // -> 2
    TEST_ASSERT_EQUAL(6, screen_minutes_to_index(1000, kDimMinutesSteps, kDimMinutesStepCount)); // -> 60
    TEST_ASSERT_EQUAL(7, screen_minutes_to_index(-4, kDimMinutesSteps, kDimMinutesStepCount));   // Never
    TEST_ASSERT_EQUAL(0, screen_minutes_to_index(5, kOffMinutesSteps, kOffMinutesStepCount));    // -> 10
    TEST_ASSERT_EQUAL(5, screen_minutes_to_index(1000, kOffMinutesSteps, kOffMinutesStepCount)); // -> 480
    TEST_ASSERT_EQUAL(6, screen_minutes_to_index(0, kOffMinutesSteps, kOffMinutesStepCount));
}

// ---- ms ----
static void test_minutes_to_ms() {
    TEST_ASSERT_EQUAL_UINT32(0, screen_minutes_to_ms(0));
    TEST_ASSERT_EQUAL_UINT32(0, screen_minutes_to_ms(-1));
    TEST_ASSERT_EQUAL_UINT32(0, screen_minutes_to_ms(INT32_MIN));
    TEST_ASSERT_EQUAL_UINT32(60000u, screen_minutes_to_ms(1));
    TEST_ASSERT_EQUAL_UINT32(300000u, screen_minutes_to_ms(5));
    TEST_ASSERT_EQUAL_UINT32(480u * 60000u, screen_minutes_to_ms(480));
    TEST_ASSERT_EQUAL_UINT32(28800000u, screen_minutes_to_ms(480));
}

// ---- labels ----
static void check_labels(const char* dim_exp[], const char* off_exp[]) {
    char buf[32];
    for (int i = 0; i < kDimMinutesStepCount; i++) {
        const char* r = screen_dim_label(buf, sizeof(buf), kDimMinutesSteps[i]);
        TEST_ASSERT_TRUE(r == buf);
        TEST_ASSERT_EQUAL_STRING(dim_exp[i], buf);
    }
    for (int i = 0; i < kOffMinutesStepCount; i++) {
        const char* r = screen_off_label(buf, sizeof(buf), kOffMinutesSteps[i]);
        TEST_ASSERT_TRUE(r == buf);
        TEST_ASSERT_EQUAL_STRING(off_exp[i], buf);
    }
}

static void test_labels_every_step() {
    const char* dim_exp[] = {"Dim after: 1 min", "Dim after: 2 min", "Dim after: 5 min", "Dim after: 10 min",
                             "Dim after: 15 min", "Dim after: 30 min", "Dim after: 1 h", "Dim after: Never"};
    const char* off_exp[] = {"Screen off: 10 min", "Screen off: 30 min", "Screen off: 1 h", "Screen off: 2 h",
                             "Screen off: 4 h", "Screen off: 8 h", "Screen off: Never"};
    check_labels(dim_exp, off_exp);
}

static void test_labels_fit_32_bytes() {
    char buf[32];
    for (int i = 0; i < kDimMinutesStepCount; i++) {
        screen_dim_label(buf, sizeof(buf), kDimMinutesSteps[i]);
        TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
    }
    for (int i = 0; i < kOffMinutesStepCount; i++) {
        screen_off_label(buf, sizeof(buf), kOffMinutesSteps[i]);
        TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
    }
}

static void test_label_negative_is_never() {
    char buf[32];
    screen_dim_label(buf, sizeof(buf), -3);
    TEST_ASSERT_EQUAL_STRING("Dim after: Never", buf);
}

static void test_label_truncation_safe() {
    char buf[32];
    // Canary after a small window.
    memset(buf, 'X', sizeof(buf));
    const char* r = screen_dim_label(buf, 6, 5);
    TEST_ASSERT_TRUE(r == buf);
    TEST_ASSERT_EQUAL_STRING("Dim a", buf);
    TEST_ASSERT_EQUAL_CHAR('X', buf[6]);
    TEST_ASSERT_EQUAL_CHAR('X', buf[7]);

    memset(buf, 'X', sizeof(buf));
    screen_off_label(buf, 1, 30);
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
    TEST_ASSERT_EQUAL_CHAR('X', buf[1]);

    memset(buf, 'X', sizeof(buf));
    screen_off_label(buf, 2, 0);
    TEST_ASSERT_EQUAL_STRING("S", buf);
    TEST_ASSERT_EQUAL_CHAR('X', buf[2]);

    // Exact fit: "Dim after: 5 min" is 16 chars, needs 17.
    memset(buf, 'X', sizeof(buf));
    screen_dim_label(buf, 17, 5);
    TEST_ASSERT_EQUAL_STRING("Dim after: 5 min", buf);
    memset(buf, 'X', sizeof(buf));
    screen_dim_label(buf, 16, 5);
    TEST_ASSERT_EQUAL_STRING("Dim after: 5 mi", buf);
    TEST_ASSERT_EQUAL_CHAR('X', buf[16]);
}

static void test_label_out_len_zero_writes_nothing() {
    char buf[8];
    memset(buf, 'X', sizeof(buf));
    const char* r = screen_dim_label(buf, 0, 5);
    TEST_ASSERT_TRUE(r == buf);
    for (size_t i = 0; i < sizeof(buf); i++) TEST_ASSERT_EQUAL_CHAR('X', buf[i]);
    r = screen_off_label(buf, 0, 0);
    TEST_ASSERT_TRUE(r == buf);
    for (size_t i = 0; i < sizeof(buf); i++) TEST_ASSERT_EQUAL_CHAR('X', buf[i]);
}

void run_screen_power_tests() {
    RUN_TEST(test_decide_both_never_always_awake);
    RUN_TEST(test_decide_dim_only);
    RUN_TEST(test_decide_off_only_goes_awake_to_off_directly);
    RUN_TEST(test_decide_dim_then_off_thresholds);
    RUN_TEST(test_decide_off_not_after_dim_off_wins);
    RUN_TEST(test_decide_keep_awake_beats_everything);
    RUN_TEST(test_decide_keep_on_blocks_off_only);
    RUN_TEST(test_decide_idle_is_min_of_touch_and_weight);
    RUN_TEST(test_decide_weak_weight_does_not_wake_from_off);
    RUN_TEST(test_decide_strong_weight_wakes_from_off);
    RUN_TEST(test_decide_weak_weight_counts_when_not_off);
    RUN_TEST(test_decide_touch_wakes_from_off);
    RUN_TEST(test_decide_off_stays_off_without_activity);
    RUN_TEST(test_state_allows_off_all_states);
    RUN_TEST(test_error_blocks_off);
    RUN_TEST(test_block_off_always_blocked);
    RUN_TEST(test_block_wake_with_touch_held_then_release);
    RUN_TEST(test_block_wake_to_dimmed_also_swallows);
    RUN_TEST(test_block_wake_by_weight_no_touch_clears_at_once);
    RUN_TEST(test_block_never_blocks_when_not_off);
    RUN_TEST(test_storage_normalise);
    RUN_TEST(test_api_never_cases);
    RUN_TEST(test_api_nearest_and_ties);
    RUN_TEST(test_api_above_largest_is_largest);
    RUN_TEST(test_api_every_listed_step_maps_to_itself);
    RUN_TEST(test_steps_tables);
    RUN_TEST(test_index_round_trip);
    RUN_TEST(test_index_clamped);
    RUN_TEST(test_to_index_unlisted_maps_to_nearest);
    RUN_TEST(test_minutes_to_ms);
    RUN_TEST(test_labels_every_step);
    RUN_TEST(test_labels_fit_32_bytes);
    RUN_TEST(test_label_negative_is_never);
    RUN_TEST(test_label_truncation_safe);
    RUN_TEST(test_label_out_len_zero_writes_nothing);
}
