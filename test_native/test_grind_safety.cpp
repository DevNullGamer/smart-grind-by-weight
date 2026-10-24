#include <unity.h>
#include <limits>
#include "controllers/grind_safety.h"
#include "config/grind_control.h"

static void test_timed_out_normal() {
    TEST_ASSERT_FALSE(grind_phase_timed_out(1500, 1000, 1000));
    TEST_ASSERT_TRUE(grind_phase_timed_out(3000, 1000, 1000));
}

static void test_timed_out_boundary() {
    TEST_ASSERT_FALSE(grind_phase_timed_out(1999, 1000, 1000));
    TEST_ASSERT_TRUE(grind_phase_timed_out(2000, 1000, 1000));
    TEST_ASSERT_TRUE(grind_phase_timed_out(2001, 1000, 1000));
}

static void test_timed_out_now_before_start() {
    // start ahead of now counts as 0 elapsed: only a zero limit fires
    TEST_ASSERT_FALSE(grind_phase_timed_out(1000, 1005, 1));
    TEST_ASSERT_FALSE(grind_phase_timed_out(1000, 1005, 5000));
    // across the wrap as well
    TEST_ASSERT_FALSE(grind_phase_timed_out(0xFFFFFFFEu, 3u, 1));
}

static void test_timed_out_wrap() {
    // start 0xFFFFFF00, now 0x000000FF: 511 ms elapsed
    TEST_ASSERT_FALSE(grind_phase_timed_out(0x000000FFu, 0xFFFFFF00u, 512));
    TEST_ASSERT_TRUE(grind_phase_timed_out(0x000000FFu, 0xFFFFFF00u, 511));
    TEST_ASSERT_TRUE(grind_phase_timed_out(0x000000FFu, 0xFFFFFF00u, 100));
}

static void test_prime_no_flow_before_check_time() {
    TEST_ASSERT_FALSE(grind_prime_no_flow(5999, 1000, 0.0f, 1.0f, 5000, 50));
    TEST_ASSERT_FALSE(grind_prime_no_flow(1000, 1000, 0.0f, 1.0f, 5000, 50));
}

static void test_prime_no_flow_at_check_time() {
    TEST_ASSERT_TRUE(grind_prime_no_flow(6000, 1000, 0.0f, 1.0f, 5000, 50));
    TEST_ASSERT_TRUE(grind_prime_no_flow(9000, 1000, 0.49f, 1.0f, 5000, 50));
}

static void test_prime_no_flow_weight_threshold() {
    // exactly 50% of 1.0 g is enough
    TEST_ASSERT_FALSE(grind_prime_no_flow(6000, 1000, 0.5f, 1.0f, 5000, 50));
    TEST_ASSERT_FALSE(grind_prime_no_flow(6000, 1000, 0.51f, 1.0f, 5000, 50));
    TEST_ASSERT_TRUE(grind_prime_no_flow(6000, 1000, 0.49f, 1.0f, 5000, 50));
    // scales with purge amount (2.5 g -> 1.25 g)
    TEST_ASSERT_TRUE(grind_prime_no_flow(6000, 1000, 1.2f, 2.5f, 5000, 50));
    TEST_ASSERT_FALSE(grind_prime_no_flow(6000, 1000, 1.3f, 2.5f, 5000, 50));
}

static void test_prime_no_flow_start_ahead_of_now() {
    TEST_ASSERT_FALSE(grind_prime_no_flow(1000, 1005, 0.0f, 1.0f, 5000, 50));
}

static void test_prime_no_flow_uses_config_constants() {
    TEST_ASSERT_EQUAL_UINT32(5000, GRIND_PRIME_NO_FLOW_CHECK_MS);
    TEST_ASSERT_EQUAL_UINT32(50, GRIND_PRIME_NO_FLOW_MIN_PERCENT);
    TEST_ASSERT_TRUE(grind_prime_no_flow(5000, 0, 0.0f, 1.0f, GRIND_PRIME_NO_FLOW_CHECK_MS,
                                         GRIND_PRIME_NO_FLOW_MIN_PERCENT));
    TEST_ASSERT_FALSE(grind_prime_no_flow(4999, 0, 0.0f, 1.0f, GRIND_PRIME_NO_FLOW_CHECK_MS,
                                          GRIND_PRIME_NO_FLOW_MIN_PERCENT));
}

static void test_predictive_no_flow_before_check_time() {
    TEST_ASSERT_FALSE(grind_predictive_no_flow(4999, 1000, 0.0f, 4000, 0.3f));  // 3999 elapsed
    TEST_ASSERT_FALSE(grind_predictive_no_flow(1000, 1000, 0.0f, 4000, 0.3f));
}

static void test_predictive_no_flow_gain_boundary() {
    TEST_ASSERT_TRUE(grind_predictive_no_flow(5000, 1000, 0.0f, 4000, 0.3f));
    TEST_ASSERT_TRUE(grind_predictive_no_flow(5000, 1000, 0.29f, 4000, 0.3f));
    TEST_ASSERT_FALSE(grind_predictive_no_flow(5000, 1000, 0.3f, 4000, 0.3f));
    TEST_ASSERT_FALSE(grind_predictive_no_flow(5000, 1000, 0.31f, 4000, 0.3f));
    TEST_ASSERT_FALSE(grind_predictive_no_flow(9000, 1000, 5.0f, 4000, 0.3f));
}

static void test_predictive_no_flow_start_ahead_of_now() {
    TEST_ASSERT_FALSE(grind_predictive_no_flow(1000, 1005, 0.0f, 4000, 0.3f));
}

static void test_predictive_no_flow_config_constants() {
    TEST_ASSERT_EQUAL_UINT32(4000, GRIND_PREDICTIVE_NO_FLOW_CHECK_MS);
    TEST_ASSERT_EQUAL_FLOAT(0.3f, GRIND_PREDICTIVE_NO_FLOW_MIN_GAIN_G);
}

static void test_pulse_wait_limit_normal() {
    TEST_ASSERT_EQUAL_UINT32(100 + GRIND_PULSE_WAIT_MARGIN_MS,
                             grind_pulse_wait_limit_ms(100.0f, GRIND_PULSE_WAIT_MARGIN_MS));
    TEST_ASSERT_EQUAL_UINT32(1000, GRIND_PULSE_WAIT_MARGIN_MS);
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(0.0f, 1000));
    TEST_ASSERT_EQUAL_UINT32(350, grind_pulse_wait_limit_ms(250.0f, 100));
}

static void test_pulse_wait_limit_invalid_inputs() {
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    const float inf_v = std::numeric_limits<float>::infinity();
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(nan_v, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(-50.0f, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(-inf_v, 1000));
    // +infinity is non-finite -> counts as 0
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(inf_v, 1000));
}

static void test_pulse_wait_limit_huge_clamped() {
    TEST_ASSERT_EQUAL_UINT32(3600000u + 1000u, grind_pulse_wait_limit_ms(1.0e12f, 1000));
    TEST_ASSERT_EQUAL_UINT32(3600000u + 1000u, grind_pulse_wait_limit_ms(3.0e38f, 1000));
    TEST_ASSERT_EQUAL_UINT32(3600000u, grind_pulse_wait_limit_ms(3600000.0f, 0));
}

void run_grind_safety_tests() {
    RUN_TEST(test_timed_out_normal);
    RUN_TEST(test_timed_out_boundary);
    RUN_TEST(test_timed_out_now_before_start);
    RUN_TEST(test_timed_out_wrap);
    RUN_TEST(test_prime_no_flow_before_check_time);
    RUN_TEST(test_prime_no_flow_at_check_time);
    RUN_TEST(test_prime_no_flow_weight_threshold);
    RUN_TEST(test_prime_no_flow_start_ahead_of_now);
    RUN_TEST(test_prime_no_flow_uses_config_constants);
    RUN_TEST(test_predictive_no_flow_before_check_time);
    RUN_TEST(test_predictive_no_flow_gain_boundary);
    RUN_TEST(test_predictive_no_flow_start_ahead_of_now);
    RUN_TEST(test_predictive_no_flow_config_constants);
    RUN_TEST(test_pulse_wait_limit_normal);
    RUN_TEST(test_pulse_wait_limit_invalid_inputs);
    RUN_TEST(test_pulse_wait_limit_huge_clamped);
}
