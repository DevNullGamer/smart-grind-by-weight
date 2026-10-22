#include <unity.h>
#include "hardware/rmt_pulse_math.h"

void test_deadman_is_timeout_plus_margin() {
    TEST_ASSERT_EQUAL_UINT32(62000, continuous_deadman_ms(60, 2000));
}

void test_deadman_zero_margin() {
    TEST_ASSERT_EQUAL_UINT32(60000, continuous_deadman_ms(60, 0));
}

void test_deadman_zero_timeout() {
    TEST_ASSERT_EQUAL_UINT32(2000, continuous_deadman_ms(0, 2000));
    TEST_ASSERT_EQUAL_UINT32(0, continuous_deadman_ms(0, 0));
}

void test_deadman_largest_exact_value() {
    // 4294967 s = 4294967000 ms, + 295 ms fits exactly
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, continuous_deadman_ms(4294967, 295));
}

void test_deadman_saturates_instead_of_overflowing() {
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, continuous_deadman_ms(4294967, 296));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, continuous_deadman_ms(UINT32_MAX, 2000));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, continuous_deadman_ms(60, UINT32_MAX));
}

void run_rmt_pulse_math_tests() {
    RUN_TEST(test_deadman_is_timeout_plus_margin);
    RUN_TEST(test_deadman_zero_margin);
    RUN_TEST(test_deadman_zero_timeout);
    RUN_TEST(test_deadman_largest_exact_value);
    RUN_TEST(test_deadman_saturates_instead_of_overflowing);
}
