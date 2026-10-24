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

// ---- Exact pulse symbols ----

static void check_pulse(uint32_t high_us) {
    RmtSymbol out[RMT_PULSE_SYMBOL_CAPACITY];
    const size_t n = rmt_build_pulse_symbols(high_us, out, RMT_PULSE_SYMBOL_CAPACITY);
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)rmt_pulse_symbol_count(high_us), (uint32_t)n);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(64, (uint32_t)n);

    uint64_t high_sum = 0;
    uint32_t last_low = 0;
    bool low_seen = false;
    for (size_t i = 0; i < n; i++) {
        const uint32_t d[2] = {out[i].duration0, out[i].duration1};
        const uint8_t lv[2] = {out[i].level0, out[i].level1};
        for (int h = 0; h < 2; h++) {
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(RMT_MAX_HALF_US, d[h]);
            if (h == 1 && i == n - 1 && d[h] == 0) continue;  // unused end-marker half
            if (lv[h]) {
                TEST_ASSERT_FALSE_MESSAGE(low_seen, "HIGH after LOW");
                high_sum += d[h];
            } else {
                TEST_ASSERT_FALSE_MESSAGE(low_seen, "more than one LOW");
                low_seen = true;
                last_low = d[h];
                // LOW must be the final used half
                TEST_ASSERT_TRUE(i == n - 1);
            }
        }
    }
    TEST_ASSERT_TRUE(low_seen);
    TEST_ASSERT_EQUAL_UINT32(1, last_low);
    TEST_ASSERT_EQUAL_UINT32(high_us, (uint32_t)high_sum);
}

void test_pulse_sweep_1_to_4000_ms_exact() {
    for (uint32_t ms = 1; ms <= 4000; ms++) {
        check_pulse(ms * 1000u);
    }
}

void test_pulse_sweep_odd_microseconds() {
    const uint32_t vals[] = {1, 2, 32766, 32768, 32769, 65535, 65536, 65535 + 2, 99999, 3999999, 4000000};
    for (uint32_t v : vals) check_pulse(v);
}

void test_pulse_exact_multiples_of_max_half() {
    // Tail goes in its own symbol when the last symbol is full of HIGH halves
    RmtSymbol out[RMT_PULSE_SYMBOL_CAPACITY];
    // 32767 = 1 half -> tail shares its symbol
    TEST_ASSERT_EQUAL_UINT32(1, rmt_build_pulse_symbols(32767u, out, 64));
    check_pulse(32767u);
    check_pulse(65534u);
    check_pulse(98301u);
    // 65534 = 2 halves -> HIGH,HIGH symbol + own tail symbol
    TEST_ASSERT_EQUAL_UINT32(2, rmt_build_pulse_symbols(65534u, out, 64));
    TEST_ASSERT_EQUAL_UINT32(0, out[1].duration1);
    TEST_ASSERT_EQUAL_UINT32(1, out[1].duration0);
    TEST_ASSERT_EQUAL_UINT32(0, out[1].level0);
    // 98301 = 3 halves -> symbol(H,H), symbol(H,tail)
    TEST_ASSERT_EQUAL_UINT32(2, rmt_build_pulse_symbols(98301u, out, 64));
}

void test_pulse_4000ms_is_62_symbols() {
    TEST_ASSERT_EQUAL_UINT32(62, (uint32_t)rmt_pulse_symbol_count(4000000u));
    RmtSymbol out[64];
    TEST_ASSERT_EQUAL_UINT32(62, (uint32_t)rmt_build_pulse_symbols(4000000u, out, 64));
}

void test_pulse_zero_and_bad_args() {
    RmtSymbol out[64];
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rmt_pulse_symbol_count(0));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rmt_build_pulse_symbols(0, out, 64));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rmt_build_pulse_symbols(1000, nullptr, 64));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rmt_build_pulse_symbols(4000000u, out, 61));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rmt_build_pulse_symbols(1000, out, 0));
}

void run_rmt_pulse_math_tests() {
    RUN_TEST(test_pulse_sweep_1_to_4000_ms_exact);
    RUN_TEST(test_pulse_sweep_odd_microseconds);
    RUN_TEST(test_pulse_exact_multiples_of_max_half);
    RUN_TEST(test_pulse_4000ms_is_62_symbols);
    RUN_TEST(test_pulse_zero_and_bad_args);
    RUN_TEST(test_deadman_is_timeout_plus_margin);
    RUN_TEST(test_deadman_zero_margin);
    RUN_TEST(test_deadman_zero_timeout);
    RUN_TEST(test_deadman_largest_exact_value);
    RUN_TEST(test_deadman_saturates_instead_of_overflowing);
}
