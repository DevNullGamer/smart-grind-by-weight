// GLinkResultGate (chg025 3.4): which grindResult may be reported as a finished grind.
#include <stdint.h>
#include <unity.h>
#include "GLinkResultGate.h"

using namespace glinkc;

namespace {

bool result(ResultGate& g, uint32_t seq, bool settled, int32_t tenths, int32_t* out = nullptr) {
    int32_t o = -1;
    return g.on_result(seq, settled, tenths, out ? out : &o);
}

void test_result_at_or_below_floor_not_reported() {
    ResultGate g;
    g.on_ready(10);
    TEST_ASSERT_FALSE(result(g, 10, true, 181));
    TEST_ASSERT_FALSE(result(g, 9, true, 181));
    TEST_ASSERT_FALSE(result(g, 0, true, 181));
}

void test_first_newer_settled_reported_once_with_weight() {
    ResultGate g;
    g.on_ready(10);
    int32_t out = 0;
    TEST_ASSERT_TRUE(result(g, 11, true, 181, &out));
    TEST_ASSERT_EQUAL_INT32(181, out);
}

void test_same_seq_twice_reports_once() {
    ResultGate g;
    g.on_ready(10);
    TEST_ASSERT_TRUE(result(g, 11, true, 181));
    TEST_ASSERT_FALSE(result(g, 11, true, 181));
}

void test_unsettled_not_reported_but_floor_rises() {
    ResultGate g;
    g.on_ready(10);
    TEST_ASSERT_FALSE(result(g, 11, false, 181));
    TEST_ASSERT_EQUAL_UINT32(11, g.floor());
    TEST_ASSERT_FALSE(result(g, 11, true, 181));  // same seq, floor already there
}

void test_stopped_then_ok_reports_the_ok() {
    ResultGate g;
    g.on_ready(6);
    TEST_ASSERT_FALSE(result(g, 7, false, 120));
    int32_t out = 0;
    TEST_ASSERT_TRUE(result(g, 8, true, 185, &out));
    TEST_ASSERT_EQUAL_INT32(185, out);
}

void test_no_baseline_reports_nothing() {
    ResultGate g;
    TEST_ASSERT_FALSE(g.has_baseline());
    TEST_ASSERT_FALSE(result(g, 5, true, 181));
    g.on_ready(4);  // the ignored result did not move a floor
    TEST_ASSERT_TRUE(g.has_baseline());
    TEST_ASSERT_TRUE(result(g, 5, true, 181));
}

void test_on_ready_with_lower_seq_rebaselines() {
    ResultGate g;
    g.on_ready(500);
    g.reset();
    g.on_ready(0);  // rebooted grinder restarts its counter
    TEST_ASSERT_TRUE(result(g, 1, true, 181));
    // even without reset in between
    ResultGate h;
    h.on_ready(500);
    h.on_ready(2);
    TEST_ASSERT_EQUAL_UINT32(2, h.floor());
    TEST_ASSERT_TRUE(result(h, 3, true, 181));
}

void test_seq_wrap() {
    ResultGate g;
    g.on_ready(0xFFFFFFFFu);
    TEST_ASSERT_FALSE(result(g, 0xFFFFFFFFu, true, 181));
    TEST_ASSERT_TRUE(result(g, 0, true, 181));
    TEST_ASSERT_FALSE(result(g, 0xFFFFFFFFu, true, 181));  // now older than the floor
}

void test_reset_clears_baseline_and_floor() {
    ResultGate g;
    g.on_ready(10);
    TEST_ASSERT_TRUE(result(g, 11, true, 181));
    g.reset();
    TEST_ASSERT_FALSE(g.has_baseline());
    TEST_ASSERT_FALSE(result(g, 12, true, 181));  // gap grind, no baseline
    g.on_ready(12);
    TEST_ASSERT_FALSE(result(g, 12, true, 181));
    TEST_ASSERT_TRUE(result(g, 13, true, 181));
}

}  // namespace

void run_glink_plugin_resultgate_tests() {
    RUN_TEST(test_result_at_or_below_floor_not_reported);
    RUN_TEST(test_first_newer_settled_reported_once_with_weight);
    RUN_TEST(test_same_seq_twice_reports_once);
    RUN_TEST(test_unsettled_not_reported_but_floor_rises);
    RUN_TEST(test_stopped_then_ok_reports_the_ok);
    RUN_TEST(test_no_baseline_reports_nothing);
    RUN_TEST(test_on_ready_with_lower_seq_rebaselines);
    RUN_TEST(test_seq_wrap);
    RUN_TEST(test_reset_clears_baseline_and_floor);
}
