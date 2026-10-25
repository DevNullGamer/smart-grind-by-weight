#include <unity.h>
#include <string.h>
#include "controllers/grind_start_check.h"

static const GrindStartFault ALL_FAULTS[] = {GrindStartFault::NONE, GrindStartFault::NOT_CONNECTED,
                                             GrindStartFault::NO_DATA, GrindStartFault::INVALID_SAMPLE_RATE};
static const GrindStartBlock FAULT_BLOCKS[] = {GrindStartBlock::NONE, GrindStartBlock::LOAD_CELL_NOT_CONNECTED,
                                               GrindStartBlock::LOAD_CELL_NO_DATA,
                                               GrindStartBlock::LOAD_CELL_RATE_INVALID};
static const GrindStartBlock TEXT_BLOCKS[] = {GrindStartBlock::LOAD_CELL_NOT_CONNECTED,
                                              GrindStartBlock::LOAD_CELL_NO_DATA,
                                              GrindStartBlock::LOAD_CELL_RATE_INVALID,
                                              GrindStartBlock::UPDATE_IN_PROGRESS};

static void test_all_clear_is_none() {
    TEST_ASSERT_EQUAL(GrindStartBlock::NONE, grind_start_block(true, false, GrindStartFault::NONE));
}

static void test_single_faults() {
    TEST_ASSERT_EQUAL(GrindStartBlock::MOTOR_DISABLED, grind_start_block(false, false, GrindStartFault::NONE));
    TEST_ASSERT_EQUAL(GrindStartBlock::UPDATE_IN_PROGRESS, grind_start_block(true, true, GrindStartFault::NONE));
    TEST_ASSERT_EQUAL(GrindStartBlock::LOAD_CELL_NOT_CONNECTED,
                      grind_start_block(true, false, GrindStartFault::NOT_CONNECTED));
    TEST_ASSERT_EQUAL(GrindStartBlock::LOAD_CELL_NO_DATA, grind_start_block(true, false, GrindStartFault::NO_DATA));
    TEST_ASSERT_EQUAL(GrindStartBlock::LOAD_CELL_RATE_INVALID,
                      grind_start_block(true, false, GrindStartFault::INVALID_SAMPLE_RATE));
}

static void test_priority_all_combinations() {
    for (int m = 0; m < 2; m++) {
        for (int l = 0; l < 2; l++) {
            for (int f = 0; f < 4; f++) {
                GrindStartBlock expected;
                if (!m) expected = GrindStartBlock::MOTOR_DISABLED;
                else if (l) expected = GrindStartBlock::UPDATE_IN_PROGRESS;
                else expected = FAULT_BLOCKS[f];
                TEST_ASSERT_EQUAL(expected, grind_start_block(m != 0, l != 0, ALL_FAULTS[f]));
            }
        }
    }
}

static void test_texts_non_empty() {
    for (GrindStartBlock b : TEXT_BLOCKS) {
        TEST_ASSERT_NOT_NULL(grind_start_block_reason(b));
        TEST_ASSERT_TRUE(strlen(grind_start_block_reason(b)) > 0);
        TEST_ASSERT_NOT_NULL(grind_start_block_title(b));
        TEST_ASSERT_TRUE(strlen(grind_start_block_title(b)) > 0);
        TEST_ASSERT_NOT_NULL(grind_start_block_message(b));
        TEST_ASSERT_TRUE(strlen(grind_start_block_message(b)) > 0);
    }
    TEST_ASSERT_NOT_NULL(grind_start_block_reason(GrindStartBlock::MOTOR_DISABLED));
    TEST_ASSERT_TRUE(strlen(grind_start_block_reason(GrindStartBlock::MOTOR_DISABLED)) > 0);
}

static void test_none_and_motor_have_no_notice() {
    TEST_ASSERT_NOT_NULL(grind_start_block_reason(GrindStartBlock::NONE));
    TEST_ASSERT_EQUAL_STRING("", grind_start_block_reason(GrindStartBlock::NONE));
    TEST_ASSERT_NULL(grind_start_block_title(GrindStartBlock::NONE));
    TEST_ASSERT_NULL(grind_start_block_message(GrindStartBlock::NONE));
    TEST_ASSERT_NULL(grind_start_block_title(GrindStartBlock::MOTOR_DISABLED));
    TEST_ASSERT_NULL(grind_start_block_message(GrindStartBlock::MOTOR_DISABLED));
}

static void test_message_first_line_fits() {
    for (GrindStartBlock b : TEXT_BLOCKS) {
        const char* msg = grind_start_block_message(b);
        const char* nl = strchr(msg, '\n');
        size_t len = nl ? (size_t)(nl - msg) : strlen(msg);
        TEST_ASSERT_TRUE(len <= 24);
    }
}

static void test_texts_match_plan() {
    TEST_ASSERT_EQUAL_STRING("CAN'T START", grind_start_block_title(GrindStartBlock::UPDATE_IN_PROGRESS));
    TEST_ASSERT_EQUAL_STRING("CAN'T START", grind_start_block_title(GrindStartBlock::LOAD_CELL_NOT_CONNECTED));
    TEST_ASSERT_EQUAL_STRING("CAN'T START", grind_start_block_title(GrindStartBlock::LOAD_CELL_NO_DATA));
    TEST_ASSERT_EQUAL_STRING("CAN'T START", grind_start_block_title(GrindStartBlock::LOAD_CELL_RATE_INVALID));

    struct Case { GrindStartBlock b; const char* first; };
    const Case cases[] = {
        {GrindStartBlock::LOAD_CELL_NOT_CONNECTED, "Load cell not connected"},
        {GrindStartBlock::LOAD_CELL_NO_DATA, "Load cell sends no data"},
        {GrindStartBlock::LOAD_CELL_RATE_INVALID, "Load cell rate invalid"},
        {GrindStartBlock::UPDATE_IN_PROGRESS, "Update in progress"},
    };
    for (const Case& c : cases) {
        const char* msg = grind_start_block_message(c.b);
        size_t n = strlen(c.first);
        TEST_ASSERT_EQUAL_INT(0, strncmp(msg, c.first, n));
        TEST_ASSERT_EQUAL_CHAR('\n', msg[n]);
    }
}

void run_grind_start_check_tests() {
    RUN_TEST(test_all_clear_is_none);
    RUN_TEST(test_single_faults);
    RUN_TEST(test_priority_all_combinations);
    RUN_TEST(test_texts_non_empty);
    RUN_TEST(test_none_and_motor_have_no_notice);
    RUN_TEST(test_message_first_line_fits);
    RUN_TEST(test_texts_match_plan);
}
