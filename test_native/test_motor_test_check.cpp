#include <unity.h>
#include <string.h>
#include "controllers/motor_test_check.h"
#include "controllers/grind_start_check.h"

static const MotorTestBlock ALL_BLOCKS[] = {MotorTestBlock::NONE, MotorTestBlock::MOTOR_DISABLED,
                                            MotorTestBlock::UPDATE_IN_PROGRESS, MotorTestBlock::BUSY,
                                            MotorTestBlock::MOTOR_FAULT, MotorTestBlock::START_FAILED};
static const MotorTestOutcome ALL_OUTCOMES[] = {MotorTestOutcome::OK, MotorTestOutcome::ENDED_EARLY,
                                                MotorTestOutcome::NOT_STOPPED, MotorTestOutcome::STOPPED_BY_USER};

static void test_block_all_clear_is_none() {
    TEST_ASSERT_EQUAL(MotorTestBlock::NONE, motor_test_block(true, false, false, false));
}

static void test_block_priority_all_combinations() {
    for (int a = 0; a < 2; a++) {
        for (int l = 0; l < 2; l++) {
            for (int b = 0; b < 2; b++) {
                for (int f = 0; f < 2; f++) {
                    MotorTestBlock expected;
                    if (f) expected = MotorTestBlock::MOTOR_FAULT;
                    else if (!a) expected = MotorTestBlock::MOTOR_DISABLED;
                    else if (l) expected = MotorTestBlock::UPDATE_IN_PROGRESS;
                    else if (b) expected = MotorTestBlock::BUSY;
                    else expected = MotorTestBlock::NONE;
                    TEST_ASSERT_EQUAL(expected, motor_test_block(a != 0, l != 0, b != 0, f != 0));
                }
            }
        }
    }
}

static void test_block_never_returns_start_failed() {
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_TRUE(motor_test_block(i & 1, i & 2, i & 4, i & 8) != MotorTestBlock::START_FAILED);
    }
}

static void test_block_texts_never_null() {
    for (MotorTestBlock b : ALL_BLOCKS) {
        TEST_ASSERT_NOT_NULL(motor_test_block_reason(b));
        TEST_ASSERT_NOT_NULL(motor_test_block_title(b));
        TEST_ASSERT_NOT_NULL(motor_test_block_message(b));
        if (b != MotorTestBlock::NONE) {
            TEST_ASSERT_TRUE(strlen(motor_test_block_reason(b)) > 0);
            TEST_ASSERT_TRUE(strlen(motor_test_block_title(b)) > 0);
            TEST_ASSERT_TRUE(strlen(motor_test_block_message(b)) > 0);
        }
    }
}

static void test_block_none_texts_empty() {
    TEST_ASSERT_EQUAL_STRING("", motor_test_block_reason(MotorTestBlock::NONE));
    TEST_ASSERT_EQUAL_STRING("", motor_test_block_title(MotorTestBlock::NONE));
    TEST_ASSERT_EQUAL_STRING("", motor_test_block_message(MotorTestBlock::NONE));
}

static void test_lockout_reuses_grind_start_text() {
    TEST_ASSERT_EQUAL_STRING(grind_start_block_title(GrindStartBlock::UPDATE_IN_PROGRESS),
                             motor_test_block_title(MotorTestBlock::UPDATE_IN_PROGRESS));
    TEST_ASSERT_EQUAL_STRING(grind_start_block_message(GrindStartBlock::UPDATE_IN_PROGRESS),
                             motor_test_block_message(MotorTestBlock::UPDATE_IN_PROGRESS));
}

static void test_finish_basic() {
    TEST_ASSERT_EQUAL(MotorTestOutcome::OK, motor_test_finish(1000, 1000, true, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::NOT_STOPPED, motor_test_finish(1000, 1000, false, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::STOPPED_BY_USER, motor_test_finish(1000, 500, false, true));
}

static void test_finish_ninety_percent_boundary() {
    TEST_ASSERT_EQUAL(MotorTestOutcome::ENDED_EARLY, motor_test_finish(1000, 899, true, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::OK, motor_test_finish(1000, 900, true, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::ENDED_EARLY, motor_test_finish(1000, 0, true, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::OK, motor_test_finish(1000, 1500, true, false));
}

static void test_finish_user_stop_wins() {
    TEST_ASSERT_EQUAL(MotorTestOutcome::STOPPED_BY_USER, motor_test_finish(1000, 1000, true, true));
    TEST_ASSERT_EQUAL(MotorTestOutcome::STOPPED_BY_USER, motor_test_finish(1000, 100, true, true));
    TEST_ASSERT_EQUAL(MotorTestOutcome::STOPPED_BY_USER, motor_test_finish(1000, 100, false, true));
}

static void test_finish_no_overflow() {
    TEST_ASSERT_EQUAL(MotorTestOutcome::OK, motor_test_finish(UINT32_MAX, UINT32_MAX, true, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::ENDED_EARLY, motor_test_finish(UINT32_MAX, 1, true, false));
    // Elapsed far beyond a small request must not wrap into "early"
    TEST_ASSERT_EQUAL(MotorTestOutcome::OK, motor_test_finish(1000, UINT32_MAX, true, false));
    // 90 % of UINT32_MAX = 3865470565.5
    TEST_ASSERT_EQUAL(MotorTestOutcome::OK, motor_test_finish(UINT32_MAX, 3865470566u, true, false));
    TEST_ASSERT_EQUAL(MotorTestOutcome::ENDED_EARLY, motor_test_finish(UINT32_MAX, 3865470565u, true, false));
}

static void test_severity() {
    TEST_ASSERT_EQUAL(MotorTestSeverity::SUCCESS, motor_test_outcome_severity(MotorTestOutcome::OK));
    TEST_ASSERT_EQUAL(MotorTestSeverity::WARNING, motor_test_outcome_severity(MotorTestOutcome::ENDED_EARLY));
    TEST_ASSERT_EQUAL(MotorTestSeverity::ERROR, motor_test_outcome_severity(MotorTestOutcome::NOT_STOPPED));
    TEST_ASSERT_EQUAL(MotorTestSeverity::WARNING, motor_test_outcome_severity(MotorTestOutcome::STOPPED_BY_USER));
}

static void test_outcome_title_non_null() {
    for (MotorTestOutcome o : ALL_OUTCOMES) {
        TEST_ASSERT_NOT_NULL(motor_test_outcome_title(o));
        TEST_ASSERT_TRUE(strlen(motor_test_outcome_title(o)) > 0);
    }
}

static void test_message_ok_formats_one_decimal() {
    char buf[128];
    size_t n = motor_test_outcome_message(MotorTestOutcome::OK, 1000, 1000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(strlen(buf), n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "Output ran 1.0 s."));
}

static void test_message_early_formats() {
    char buf[128];
    size_t n = motor_test_outcome_message(MotorTestOutcome::ENDED_EARLY, 1000, 449, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(strlen(buf), n);
    TEST_ASSERT_EQUAL_STRING("Output ended after 0.4 s instead of 1.0 s.", buf);
    motor_test_outcome_message(MotorTestOutcome::ENDED_EARLY, 1000, 450, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Output ended after 0.5 s instead of 1.0 s.", buf);
}

static void test_message_fixed_texts() {
    char buf[128];
    motor_test_outcome_message(MotorTestOutcome::STOPPED_BY_USER, 1000, 100, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Stopped.", buf);
    motor_test_outcome_message(MotorTestOutcome::NOT_STOPPED, 1000, 1500, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "did not stop"));
}

static void test_message_non_empty_for_all() {
    char buf[128];
    for (MotorTestOutcome o : ALL_OUTCOMES) {
        size_t n = motor_test_outcome_message(o, 1000, 1000, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_TRUE(strlen(buf) > 0);
    }
}

static void test_message_null_or_zero_len() {
    char buf[8] = {'x', 0};
    TEST_ASSERT_EQUAL(0, motor_test_outcome_message(MotorTestOutcome::OK, 1000, 1000, nullptr, 64));
    TEST_ASSERT_EQUAL(0, motor_test_outcome_message(MotorTestOutcome::OK, 1000, 1000, buf, 0));
    TEST_ASSERT_EQUAL_CHAR('x', buf[0]);
}

static void test_message_bounded() {
    for (MotorTestOutcome o : ALL_OUTCOMES) {
        for (size_t len = 1; len <= 12; len++) {
            char guarded[40];
            memset(guarded, 0x7F, sizeof(guarded));
            size_t n = motor_test_outcome_message(o, 1000, 449, guarded, len);
            TEST_ASSERT_TRUE(n <= len - 1);
            TEST_ASSERT_EQUAL(n, strlen(guarded));
            TEST_ASSERT_TRUE(strlen(guarded) < len);
            for (size_t i = len; i < sizeof(guarded); i++) TEST_ASSERT_EQUAL_CHAR(0x7F, guarded[i]);
        }
    }
}

static void test_message_buffer_of_one() {
    char buf[1] = {'x'};
    TEST_ASSERT_EQUAL(0, motor_test_outcome_message(MotorTestOutcome::OK, 1000, 1000, buf, 1));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
}

static void test_message_extreme_values_fit() {
    char buf[200];
    for (MotorTestOutcome o : ALL_OUTCOMES) {
        size_t n = motor_test_outcome_message(o, UINT32_MAX, UINT32_MAX, buf, sizeof(buf));
        TEST_ASSERT_TRUE(n > 0);
        TEST_ASSERT_EQUAL(strlen(buf), n);
    }
}

void run_motor_test_check_tests() {
    RUN_TEST(test_block_all_clear_is_none);
    RUN_TEST(test_block_priority_all_combinations);
    RUN_TEST(test_block_never_returns_start_failed);
    RUN_TEST(test_block_texts_never_null);
    RUN_TEST(test_block_none_texts_empty);
    RUN_TEST(test_lockout_reuses_grind_start_text);
    RUN_TEST(test_finish_basic);
    RUN_TEST(test_finish_ninety_percent_boundary);
    RUN_TEST(test_finish_user_stop_wins);
    RUN_TEST(test_finish_no_overflow);
    RUN_TEST(test_severity);
    RUN_TEST(test_outcome_title_non_null);
    RUN_TEST(test_message_ok_formats_one_decimal);
    RUN_TEST(test_message_early_formats);
    RUN_TEST(test_message_fixed_texts);
    RUN_TEST(test_message_non_empty_for_all);
    RUN_TEST(test_message_null_or_zero_len);
    RUN_TEST(test_message_bounded);
    RUN_TEST(test_message_buffer_of_one);
    RUN_TEST(test_message_extreme_values_fit);
}
