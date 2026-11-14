// Dose save policy (GLinkDoseSave, chg024 part B): pure state machine with an injected clock.
#include <stdint.h>
#include <unity.h>
#include "GLinkDoseSave.h"

using namespace glinkc;

namespace {

constexpr uint32_t EXPIRY = DoseSaveLimits::PENDING_MS;

// A DoseSave that has a baseline at `seq`.
DoseSave based(uint32_t seq) {
    DoseSave d;
    d.on_ready(seq);
    return d;
}

bool take_now(DoseSave& d, uint32_t now, int32_t* t = nullptr, uint32_t* s = nullptr) {
    int32_t tt = 0;
    uint32_t ss = 0;
    return d.take(true, now, t ? t : &tt, s ? s : &ss);
}

void test_no_write_before_baseline() {
    DoseSave d;
    d.on_result(5, true, 181, true, 0);
    TEST_ASSERT_FALSE(d.has_pending());
    // the ignored result did not move a floor: a baseline of 4 still accepts seq 5
    d.on_ready(4);
    d.on_result(5, true, 181, true, 0);
    TEST_ASSERT_TRUE(d.has_pending());
}

void test_seq_equal_to_baseline_is_not_written() {
    DoseSave d = based(10);
    d.on_result(10, true, 181, true, 0);
    TEST_ASSERT_FALSE(d.has_pending());
    d.on_result(9, true, 181, true, 0);
    TEST_ASSERT_FALSE(d.has_pending());
}

void test_next_result_becomes_pending_and_is_taken_once() {
    DoseSave d = based(10);
    d.on_result(11, true, 181, true, 100);
    TEST_ASSERT_TRUE(d.has_pending());
    int32_t t = 0;
    uint32_t s = 0;
    TEST_ASSERT_TRUE(take_now(d, 200, &t, &s));
    TEST_ASSERT_EQUAL_INT32(181, t);
    TEST_ASSERT_EQUAL_UINT32(11, s);
    TEST_ASSERT_FALSE(take_now(d, 201));
    TEST_ASSERT_EQUAL_INT32(181, d.last_saved_tenths());
    TEST_ASSERT_EQUAL_UINT32(11, d.last_saved_seq());
}

void test_same_seq_redelivered_is_not_written_twice() {
    DoseSave d = based(10);
    d.on_result(11, true, 181, true, 0);
    TEST_ASSERT_TRUE(take_now(d, 1));
    d.on_result(11, true, 181, true, 2);
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(take_now(d, 3));
}

void test_settled_results_write() {
    DoseSave d = based(0);
    for (uint32_t seq = 1; seq <= 3; seq++) {  // OK, OVERSHOOT, MAX_PULSES arrive as weight_settled true
        d.on_result(seq, true, 100 + (int32_t)seq, true, 0);
        int32_t t = 0;
        TEST_ASSERT_TRUE(take_now(d, 1, &t));
        TEST_ASSERT_EQUAL_INT32(100 + (int32_t)seq, t);
    }
}

void test_unsettled_results_do_not_write_but_advance_the_floor() {
    DoseSave d = based(0);
    d.on_result(1, false, 181, true, 0);  // STOPPED / TIMEOUT / ERR / UPDATE
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::NOT_SETTLED);
    d.on_result(1, true, 181, true, 0);  // same seq again: floor already there
    TEST_ASSERT_FALSE(d.has_pending());
}

void test_stopped_then_ok_writes_the_later_seq() {
    DoseSave d = based(6);
    d.on_result(7, false, 120, true, 0);
    d.on_result(8, true, 185, true, 5);
    int32_t t = 0;
    uint32_t s = 0;
    TEST_ASSERT_TRUE(take_now(d, 6, &t, &s));
    TEST_ASSERT_EQUAL_INT32(185, t);
    TEST_ASSERT_EQUAL_UINT32(8, s);
}

void test_range_limits() {
    const int32_t bad[] = {49, 0, -10, 401, 600};
    for (int32_t v : bad) {
        DoseSave d = based(0);
        d.on_result(1, true, v, true, 0);
        TEST_ASSERT_FALSE(d.has_pending());
        TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::OUT_OF_RANGE);
    }
    const int32_t good[] = {50, 400};
    for (int32_t v : good) {
        DoseSave d = based(0);
        d.on_result(1, true, v, true, 0);
        int32_t t = 0;
        TEST_ASSERT_TRUE(take_now(d, 1, &t));
        TEST_ASSERT_EQUAL_INT32(v, t);
    }
}

void test_disabled_at_result_time_advances_floor_and_never_writes_later() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, false, 0);
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::FEATURE_OFF);
    d.on_result(1, true, 181, true, 1);  // enabled later, same old result redelivered
    TEST_ASSERT_FALSE(d.has_pending());
    d.on_result(2, true, 190, true, 2);
    TEST_ASSERT_TRUE(d.has_pending());
}

void test_cancel_drops_pending() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    d.cancel();
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(take_now(d, 1));
}

void test_link_down_clears_everything_and_gap_grind_is_not_written() {
    DoseSave d = based(10);
    d.on_result(11, true, 181, true, 0);
    d.on_link_down();
    TEST_ASSERT_FALSE(d.has_pending());
    d.on_result(12, true, 190, true, 1);  // no baseline now
    TEST_ASSERT_FALSE(d.has_pending());
    d.on_ready(12);  // the next link's baseline already includes the grind made during the gap
    d.on_result(12, true, 190, true, 2);
    TEST_ASSERT_FALSE(d.has_pending());
    d.on_result(13, true, 195, true, 3);
    TEST_ASSERT_TRUE(d.has_pending());
}

void test_grinder_reboot_rebaselines_to_a_lower_seq() {
    DoseSave d = based(500);
    d.on_link_down();
    d.on_ready(0);  // rebooted grinder restarts its counter
    d.on_result(1, true, 181, true, 0);
    int32_t t = 0;
    TEST_ASSERT_TRUE(take_now(d, 1, &t));
    TEST_ASSERT_EQUAL_INT32(181, t);
}

void test_pending_is_kept_while_the_machine_cannot_apply() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 1000);
    int32_t t = 0;
    uint32_t s = 0;
    TEST_ASSERT_FALSE(d.take(false, 1001, &t, &s));
    TEST_ASSERT_FALSE(d.take(false, 1000 + EXPIRY - 1, &t, &s));
    TEST_ASSERT_TRUE(d.has_pending());
    TEST_ASSERT_TRUE(d.take(true, 1000 + EXPIRY - 1, &t, &s));
    TEST_ASSERT_EQUAL_INT32(181, t);
    TEST_ASSERT_FALSE(d.take(true, 1000 + EXPIRY, &t, &s));
}

void test_newer_valid_result_replaces_pending() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    d.on_result(2, true, 190, true, 10);
    int32_t t = 0;
    uint32_t s = 0;
    TEST_ASSERT_TRUE(take_now(d, 11, &t, &s));
    TEST_ASSERT_EQUAL_INT32(190, t);
    TEST_ASSERT_EQUAL_UINT32(2, s);
    TEST_ASSERT_FALSE(take_now(d, 12));
}

void test_newer_invalid_result_keeps_the_older_pending() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    d.on_result(2, true, 600, true, 10);
    int32_t t = 0;
    TEST_ASSERT_TRUE(take_now(d, 11, &t));
    TEST_ASSERT_EQUAL_INT32(181, t);
}

void test_expiry_drops_the_pending_value() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    int32_t t = 0;
    uint32_t s = 0;
    TEST_ASSERT_FALSE(d.take(true, EXPIRY, &t, &s));
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_EQUAL_INT32(0, d.last_saved_tenths());
}

void test_expiry_across_the_millis_wrap() {
    const uint32_t start = 0xFFFFFFFFu - 1000u;  // pending stored shortly before the wrap
    int32_t t = 0;
    uint32_t s = 0;
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, start);
    TEST_ASSERT_FALSE(d.take(false, start + 2000u, &t, &s));
    TEST_ASSERT_TRUE(d.has_pending());  // 2 s later, past the wrap: still alive
    TEST_ASSERT_TRUE(d.take(true, start + EXPIRY - 1, &t, &s));
    DoseSave e = based(0);
    e.on_result(1, true, 181, true, start);
    TEST_ASSERT_FALSE(e.take(true, start + EXPIRY, &t, &s));
    TEST_ASSERT_FALSE(e.has_pending());
}

void test_seq_counter_wrap_is_handled() {
    DoseSave d = based(0xFFFFFFFFu);
    d.on_result(0, true, 181, true, 0);  // next value after the 32-bit counter wraps
    TEST_ASSERT_TRUE(d.has_pending());
}

void test_expiry_while_the_machine_cannot_apply() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    int32_t t = 0;
    uint32_t s = 0;
    TEST_ASSERT_FALSE(d.take(false, EXPIRY, &t, &s));  // can_apply false does not keep a value that is too old
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(d.take(true, EXPIRY + 1, &t, &s));
    TEST_ASSERT_EQUAL_INT32(0, d.last_saved_tenths());
}

void test_on_ready_clears_a_pending_value() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    d.on_ready(1);  // a new link without on_link_down in between
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(take_now(d, 1));
}

void test_last_skip_is_none_after_a_successful_take() {
    DoseSave d = based(0);
    d.on_result(1, false, 181, true, 0);
    TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::NOT_SETTLED);
    d.on_result(2, true, 190, true, 1);
    TEST_ASSERT_TRUE(take_now(d, 2));
    TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::NONE);
}

void test_link_down_keeps_the_last_saved_dose() {
    DoseSave d = based(0);
    d.on_result(1, true, 181, true, 0);
    TEST_ASSERT_TRUE(take_now(d, 1));
    d.on_link_down();
    TEST_ASSERT_EQUAL_INT32(181, d.last_saved_tenths());
    TEST_ASSERT_EQUAL_UINT32(1, d.last_saved_seq());
}

}  // namespace

void run_glink_plugin_dosesave_tests() {
    RUN_TEST(test_expiry_while_the_machine_cannot_apply);
    RUN_TEST(test_on_ready_clears_a_pending_value);
    RUN_TEST(test_last_skip_is_none_after_a_successful_take);
    RUN_TEST(test_link_down_keeps_the_last_saved_dose);
    RUN_TEST(test_no_write_before_baseline);
    RUN_TEST(test_seq_equal_to_baseline_is_not_written);
    RUN_TEST(test_next_result_becomes_pending_and_is_taken_once);
    RUN_TEST(test_same_seq_redelivered_is_not_written_twice);
    RUN_TEST(test_settled_results_write);
    RUN_TEST(test_unsettled_results_do_not_write_but_advance_the_floor);
    RUN_TEST(test_stopped_then_ok_writes_the_later_seq);
    RUN_TEST(test_range_limits);
    RUN_TEST(test_disabled_at_result_time_advances_floor_and_never_writes_later);
    RUN_TEST(test_cancel_drops_pending);
    RUN_TEST(test_link_down_clears_everything_and_gap_grind_is_not_written);
    RUN_TEST(test_grinder_reboot_rebaselines_to_a_lower_seq);
    RUN_TEST(test_pending_is_kept_while_the_machine_cannot_apply);
    RUN_TEST(test_newer_valid_result_replaces_pending);
    RUN_TEST(test_newer_invalid_result_keeps_the_older_pending);
    RUN_TEST(test_expiry_drops_the_pending_value);
    RUN_TEST(test_expiry_across_the_millis_wrap);
    RUN_TEST(test_seq_counter_wrap_is_handled);
}
