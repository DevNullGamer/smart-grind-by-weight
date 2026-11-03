#include <unity.h>
#include <math.h>
#include <string.h>
#include "hardware/calibration_math.h"

static const int32_t kMinDelta = 1000;

static void test_normal_factor() {
    float f = 0.0f;
    TEST_ASSERT_EQUAL(CalibrationResult::OK, calibration_compute_factor(50000, 0, 100.0f, kMinDelta, &f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, f);
    // Offset tare
    TEST_ASSERT_EQUAL(CalibrationResult::OK, calibration_compute_factor(60000, 10000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, f);
}

static void test_negative_factor_allowed() {
    float f = 0.0f;
    TEST_ASSERT_EQUAL(CalibrationResult::OK, calibration_compute_factor(10000, 60000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -500.0f, f);
}

static void test_signal_too_small_rejected_and_old_factor_kept() {
    float f = 123.0f;
    TEST_ASSERT_EQUAL(CalibrationResult::SIGNAL_TOO_SMALL, calibration_compute_factor(5999, 5000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_EQUAL(CalibrationResult::SIGNAL_TOO_SMALL, calibration_compute_factor(5000, 5000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_EQUAL(CalibrationResult::SIGNAL_TOO_SMALL, calibration_compute_factor(4001, 5000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_EQUAL_FLOAT(123.0f, f);
}

static void test_min_delta_boundary_is_accepted_both_signs() {
    float f = 0.0f;
    TEST_ASSERT_EQUAL(CalibrationResult::OK, calibration_compute_factor(6000, 5000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, f);
    TEST_ASSERT_EQUAL(CalibrationResult::OK, calibration_compute_factor(4000, 5000, 100.0f, kMinDelta, &f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -10.0f, f);
}

static void test_invalid_weight_rejected() {
    float f = 7.0f;
    TEST_ASSERT_EQUAL(CalibrationResult::INVALID_WEIGHT, calibration_compute_factor(50000, 0, 0.0f, kMinDelta, &f));
    TEST_ASSERT_EQUAL(CalibrationResult::INVALID_WEIGHT, calibration_compute_factor(50000, 0, -5.0f, kMinDelta, &f));
    TEST_ASSERT_EQUAL(CalibrationResult::INVALID_WEIGHT, calibration_compute_factor(50000, 0, NAN, kMinDelta, &f));
    TEST_ASSERT_EQUAL(CalibrationResult::INVALID_WEIGHT, calibration_compute_factor(50000, 0, INFINITY, kMinDelta, &f));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, f);
}

static void test_non_finite_factor_rejected() {
    float f = 7.0f;
    // Tiny weight makes the quotient overflow float
    TEST_ASSERT_EQUAL(CalibrationResult::INVALID_FACTOR,
                      calibration_compute_factor(0x7FFFFFFF, 0, 1e-38f, kMinDelta, &f));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, f);
}

static void test_zero_factor_rejected() {
    float f = 7.0f;
    // With the delta check disabled (min 0) an unchanged reading would give a zero factor
    TEST_ASSERT_EQUAL(CalibrationResult::INVALID_FACTOR,
                      calibration_compute_factor(5000, 5000, 100.0f, 0, &f));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, f);
}

static void test_extreme_raw_does_not_overflow() {
    float f = 0.0f;
    TEST_ASSERT_EQUAL(CalibrationResult::OK,
                      calibration_compute_factor(0x7FFFFFFF, -0x7FFFFFFF, 100.0f, kMinDelta, &f));
    TEST_ASSERT_TRUE(f > 0.0f);
}

static void test_null_out_pointer_tolerated() {
    TEST_ASSERT_EQUAL(CalibrationResult::OK, calibration_compute_factor(50000, 0, 100.0f, kMinDelta, nullptr));
}

static void test_failure_notices_have_text() {
    const CalibrationResult failures[] = {
        CalibrationResult::INVALID_WEIGHT, CalibrationResult::HARDWARE_FAULT, CalibrationResult::SETTLE_TIMEOUT,
        CalibrationResult::SIGNAL_TOO_SMALL, CalibrationResult::INVALID_FACTOR};
    for (CalibrationResult r : failures) {
        TEST_ASSERT_TRUE(strlen(calibration_result_title(r)) > 0);
        TEST_ASSERT_TRUE(strlen(calibration_result_message(r)) > 0);
    }
    TEST_ASSERT_EQUAL_STRING("", calibration_result_title(CalibrationResult::OK));
    TEST_ASSERT_EQUAL_STRING("", calibration_result_message(CalibrationResult::OK));
}

void run_calibration_math_tests() {
    RUN_TEST(test_normal_factor);
    RUN_TEST(test_negative_factor_allowed);
    RUN_TEST(test_signal_too_small_rejected_and_old_factor_kept);
    RUN_TEST(test_min_delta_boundary_is_accepted_both_signs);
    RUN_TEST(test_invalid_weight_rejected);
    RUN_TEST(test_non_finite_factor_rejected);
    RUN_TEST(test_zero_factor_rejected);
    RUN_TEST(test_extreme_raw_does_not_overflow);
    RUN_TEST(test_null_out_pointer_tolerated);
    RUN_TEST(test_failure_notices_have_text);
}
