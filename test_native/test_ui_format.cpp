#include <unity.h>
#include <string.h>
#include "ui/ui_format.h"

static void test_float_with_unit_has_single_g() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("12.35g", format_float_with_unit(buf, sizeof(buf), 12.3456f, 2, "g"));
    TEST_ASSERT_EQUAL_STRING("12.35g", buf);
}

static void test_float_without_unit() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("12.35", format_float_with_unit(buf, sizeof(buf), 12.3456f, 2, nullptr));
    TEST_ASSERT_EQUAL_STRING("12.35", format_float_with_unit(buf, sizeof(buf), 12.3456f, 2, ""));
}

static void test_float_negative_zero_and_rounding() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("-0.05g", format_float_with_unit(buf, sizeof(buf), -0.05f, 2, "g"));
    TEST_ASSERT_EQUAL_STRING("0.00g", format_float_with_unit(buf, sizeof(buf), 0.0f, 2, "g"));
    TEST_ASSERT_EQUAL_STRING("1.00g", format_float_with_unit(buf, sizeof(buf), 0.999f, 2, "g"));
    TEST_ASSERT_EQUAL_STRING("2.67g", format_float_with_unit(buf, sizeof(buf), 2.6749f, 2, "g"));
    TEST_ASSERT_EQUAL_STRING("2.68g", format_float_with_unit(buf, sizeof(buf), 2.6751f, 2, "g"));
}

static void test_float_decimals_clamped() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("3g", format_float_with_unit(buf, sizeof(buf), 2.6f, -4, "g"));
    TEST_ASSERT_EQUAL_STRING("1.000000", format_float_with_unit(buf, sizeof(buf), 1.0f, 99, nullptr));
}

static void test_int_with_unit() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("134 kB", format_int_with_unit(buf, sizeof(buf), 134, "kB"));
}

static void test_int_without_unit() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("134", format_int_with_unit(buf, sizeof(buf), 134, nullptr));
    TEST_ASSERT_EQUAL_STRING("134", format_int_with_unit(buf, sizeof(buf), 134, ""));
}

static void test_int_extremes() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("2147483647", format_int_with_unit(buf, sizeof(buf), 2147483647, nullptr));
    TEST_ASSERT_EQUAL_STRING("-2147483648", format_int_with_unit(buf, sizeof(buf), INT32_MIN, nullptr));
    TEST_ASSERT_EQUAL_STRING("0", format_int_with_unit(buf, sizeof(buf), 0, nullptr));
    TEST_ASSERT_EQUAL_STRING("0 kB", format_int_with_unit(buf, sizeof(buf), 0, "kB"));
}

// Guard bytes after the buffer must stay untouched and the result must be NUL-terminated for every capacity.
static void test_small_buffers_truncate_without_overflow() {
    for (size_t cap = 1; cap <= 8; cap++) {
        char raw[16];
        memset(raw, 0x5A, sizeof(raw));
        format_float_with_unit(raw, cap, 12.3456f, 2, "g");
        TEST_ASSERT_LESS_THAN(cap, strlen(raw));
        TEST_ASSERT_EQUAL_INT(0, strncmp(raw, "12.35g", strlen(raw)));
        for (size_t i = cap; i < sizeof(raw); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, (unsigned char)raw[i]);

        memset(raw, 0x5A, sizeof(raw));
        format_int_with_unit(raw, cap, 134, "kB");
        TEST_ASSERT_LESS_THAN(cap, strlen(raw));
        TEST_ASSERT_EQUAL_INT(0, strncmp(raw, "134 kB", strlen(raw)));
        for (size_t i = cap; i < sizeof(raw); i++) TEST_ASSERT_EQUAL_HEX8(0x5A, (unsigned char)raw[i]);
    }
    char one[2] = {'x', 'y'};
    format_int_with_unit(one, 1, 134, "kB");
    TEST_ASSERT_EQUAL_HEX8(0, one[0]);
    TEST_ASSERT_EQUAL_HEX8('y', one[1]);
}

static void test_zero_capacity_and_null_buffer_write_nothing() {
    char buf[4] = {'a', 'b', 'c', 'd'};
    TEST_ASSERT_EQUAL_PTR(buf, format_int_with_unit(buf, 0, 5, "kB"));
    TEST_ASSERT_EQUAL_PTR(buf, format_float_with_unit(buf, 0, 5.0f, 2, "g"));
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "abcd", 4));
    TEST_ASSERT_NULL(format_int_with_unit(nullptr, 8, 5, "kB"));
    TEST_ASSERT_NULL(format_float_with_unit(nullptr, 8, 5.0f, 2, "g"));
}

static void test_unit_longer_than_buffer() {
    char buf[6];
    memset(buf, 0x5A, sizeof(buf));
    format_float_with_unit(buf, sizeof(buf), 1.5f, 2, "averyverylongunit");
    TEST_ASSERT_EQUAL_STRING("1.50a", buf);
    format_int_with_unit(buf, sizeof(buf), 7, "averyverylongunit");
    TEST_ASSERT_EQUAL_STRING("7 ave", buf);
}

void run_ui_format_tests() {
    RUN_TEST(test_float_with_unit_has_single_g);
    RUN_TEST(test_float_without_unit);
    RUN_TEST(test_float_negative_zero_and_rounding);
    RUN_TEST(test_float_decimals_clamped);
    RUN_TEST(test_int_with_unit);
    RUN_TEST(test_int_without_unit);
    RUN_TEST(test_int_extremes);
    RUN_TEST(test_small_buffers_truncate_without_overflow);
    RUN_TEST(test_zero_capacity_and_null_buffer_write_nothing);
    RUN_TEST(test_unit_longer_than_buffer);
}
