#include <unity.h>
#include "hardware/sensor_health.h"

static void test_age_simple() {
    TEST_ASSERT_EQUAL_UINT32(0, sensor_sample_age_ms(1000, 1000));
    TEST_ASSERT_EQUAL_UINT32(250, sensor_sample_age_ms(1250, 1000));
}

static void test_age_across_millis_wrap() {
    // newest at 0xFFFFFF00, now wrapped to 0x000000FF: 255 + 256 = 511 ms
    TEST_ASSERT_EQUAL_UINT32(511, sensor_sample_age_ms(0x000000FFu, 0xFFFFFF00u));
    TEST_ASSERT_EQUAL_UINT32(1, sensor_sample_age_ms(0u, 0xFFFFFFFFu));
}

static void test_age_ts_ahead_of_now_clamped() {
    TEST_ASSERT_EQUAL_UINT32(0, sensor_sample_age_ms(1000, 1005));
    // Ahead across the wrap as well
    TEST_ASSERT_EQUAL_UINT32(0, sensor_sample_age_ms(0xFFFFFFFEu, 3u));
}

static void test_stale_boundary() {
    TEST_ASSERT_FALSE(sensor_sample_is_stale(true, 0, 500));
    TEST_ASSERT_FALSE(sensor_sample_is_stale(true, 500, 500));
    TEST_ASSERT_TRUE(sensor_sample_is_stale(true, 501, 500));
}

static void test_no_sample_is_stale() {
    TEST_ASSERT_TRUE(sensor_sample_is_stale(false, 0, 500));
    TEST_ASSERT_TRUE(sensor_sample_is_stale(false, 100, 500));
}

static void test_min_samples() {
    TEST_ASSERT_EQUAL_UINT32(2, settle_min_samples(200, 10, 60, 2));  // 1.2 -> 1, floor 2
    TEST_ASSERT_EQUAL_UINT32(3, settle_min_samples(500, 10, 60, 2));  // 5 * 60% = 3
    TEST_ASSERT_EQUAL_UINT32(2, settle_min_samples(0, 10, 60, 2));    // floor
}

static void test_settle_check() {
    // too few samples
    TEST_ASSERT_FALSE(settle_check(2, 3, false, 0.0f, 1.0f));
    TEST_ASSERT_FALSE(settle_check(0, 2, false, 0.0f, 1.0f));
    // enough samples, fresh, low spread
    TEST_ASSERT_TRUE(settle_check(3, 3, false, 0.5f, 1.0f));
    // stale never settles
    TEST_ASSERT_FALSE(settle_check(10, 3, true, 0.0f, 1.0f));
    // std_dev == threshold counts as settled
    TEST_ASSERT_TRUE(settle_check(3, 3, false, 1.0f, 1.0f));
    // above threshold
    TEST_ASSERT_FALSE(settle_check(3, 3, false, 1.5f, 1.0f));
}

void run_sensor_health_tests() {
    RUN_TEST(test_age_simple);
    RUN_TEST(test_age_across_millis_wrap);
    RUN_TEST(test_age_ts_ahead_of_now_clamped);
    RUN_TEST(test_stale_boundary);
    RUN_TEST(test_no_sample_is_stale);
    RUN_TEST(test_min_samples);
    RUN_TEST(test_settle_check);
}
