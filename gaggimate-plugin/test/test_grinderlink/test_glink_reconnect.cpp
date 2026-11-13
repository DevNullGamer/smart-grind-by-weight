// Auto-reconnect policy (GLinkReconnect, chg023 part 2): pure state machine with an injected clock.
#include <stdint.h>
#include <unity.h>
#include "GLinkReconnect.h"

using namespace glinkc;

namespace {

constexpr uint32_t S3 = ReconnectTiming::BACKOFF_MS[0];
constexpr uint32_t S10 = ReconnectTiming::BACKOFF_MS[1];
constexpr uint32_t S30 = ReconnectTiming::BACKOFF_MS[2];
constexpr uint32_t STABLE = ReconnectTiming::STABLE_MS;
constexpr uint32_t SETTLE = ReconnectTiming::IDLE_SETTLE_MS;

// Eligible: grinder stored, link down, no scan, IDLE.
Inputs ready() { return Inputs{true, false, false, GaggimateState::IDLE}; }
Inputs with_state(GaggimateState g) { Inputs i = ready(); i.gaggimate = g; return i; }
Inputs link_up() { Inputs i = ready(); i.link_up = true; return i; }

#define EXPECT_WAIT(r, in, t) TEST_ASSERT_TRUE((r).poll((in), (t)) == Action::WAIT)
#define EXPECT_ATTEMPT(r, in, t) TEST_ASSERT_TRUE((r).poll((in), (t)) == Action::ATTEMPT)

// Nothing fires 1 ms before `deadline`, ATTEMPT exactly at it. Caller passes the time of the event that scheduled it.
#define EXPECT_FIRES_AT(r, in, deadline) \
    do { EXPECT_WAIT((r), (in), (uint32_t)((deadline) - 1)); EXPECT_ATTEMPT((r), (in), (uint32_t)(deadline)); } while (0)

// Time of the first ATTEMPT polling every millisecond from `from`; 0 when none within `limit` ms (offset returned in ms).
// Returns the offset from `from` (limit + 1 when none).
uint32_t first_attempt_offset(Reconnect& r, const Inputs& in, uint32_t from, uint32_t limit) {
    for (uint32_t d = 0; d <= limit; d++) {
        if (r.poll(in, from + d) == Action::ATTEMPT) return d;
    }
    return limit + 1;
}

// A fresh object that has taken its boot attempt and failed it, so the next one is due after step 0 (3 s).
// Leaves backoff_step() == 1.
void boot_attempt_failed(Reconnect& r, uint32_t t) {
    EXPECT_ATTEMPT(r, ready(), t);
    r.note_attempt(t, false);
}

// ---- 1 ---------------------------------------------------------------------------------------------------------

void test_fresh_object_attempts_at_once() {
    Reconnect r;
    TEST_ASSERT_FALSE(r.user_disconnected());
    TEST_ASSERT_EQUAL_INT(0, r.backoff_step());
    EXPECT_ATTEMPT(r, ready(), 0);
}

void test_fresh_object_attempts_at_any_clock_value() {
    Reconnect r;
    EXPECT_ATTEMPT(r, ready(), 0xFFFFFFF0u);
}

void test_single_ineligibility_waits() {
    {
        Reconnect r;
        Inputs i = ready();
        i.grinder_stored = false;
        EXPECT_WAIT(r, i, 0);
        EXPECT_WAIT(r, i, 3600000u);
    }
    {
        Reconnect r;
        EXPECT_WAIT(r, link_up(), 0);
    }
    {
        Reconnect r;
        Inputs i = ready();
        i.scanning = true;
        EXPECT_WAIT(r, i, 0);
    }
    {
        Reconnect r;
        EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 0);
    }
    {
        Reconnect r;
        EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), 0);
    }
    {
        Reconnect r;
        r.note_user_disconnect();
        EXPECT_WAIT(r, ready(), 0);
    }
}

// ---- 2 ---------------------------------------------------------------------------------------------------------

void run_loss_sequence(uint32_t base) {
    Reconnect r;
    r.note_connected(base);
    const uint32_t lost = base + 1000;  // unstable
    r.note_link_lost(lost);
    EXPECT_WAIT(r, ready(), lost);
    EXPECT_FIRES_AT(r, ready(), lost + S3);
    uint32_t t = lost + S3;
    r.note_attempt(t, false);
    EXPECT_FIRES_AT(r, ready(), t + S10);
    t += S10;
    r.note_attempt(t, false);
    EXPECT_FIRES_AT(r, ready(), t + S30);
    t += S30;
    r.note_attempt(t, false);
    EXPECT_FIRES_AT(r, ready(), t + S30);
    t += S30;
    r.note_attempt(t, false);
    EXPECT_FIRES_AT(r, ready(), t + S30);
}

void test_loss_backoff_sequence() { run_loss_sequence(0); }
void test_loss_backoff_sequence_across_wrap() { run_loss_sequence(0xFFFFFFF0u); }

// ---- 3 ---------------------------------------------------------------------------------------------------------

void run_stability(uint32_t base) {
    {  // 29999 ms up: the step is kept (one failure before, so the step is 1 and the loss uses 10 s)
        Reconnect r;
        boot_attempt_failed(r, base);
        TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
        r.note_attempt(base + S3, true);
        const uint32_t lost = base + S3 + (STABLE - 1);
        r.note_link_lost(lost);
        TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
        EXPECT_WAIT(r, ready(), lost + S3);
        EXPECT_FIRES_AT(r, ready(), lost + S10);
    }
    {  // exactly 30000 ms up: reset, so 3 s and the step goes 0 -> 1
        Reconnect r;
        boot_attempt_failed(r, base);
        r.note_attempt(base + S3, true);
        const uint32_t lost = base + S3 + STABLE;
        r.note_link_lost(lost);
        TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
        EXPECT_FIRES_AT(r, ready(), lost + S3);
    }
    {  // loss without a preceding note_connected is unstable
        Reconnect r;
        boot_attempt_failed(r, base);
        r.note_link_lost(base + 100000u);
        TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
        EXPECT_FIRES_AT(r, ready(), base + 100000u + S10);
    }
}

void test_stability_threshold() { run_stability(0); }
void test_stability_threshold_across_wrap() { run_stability(0xFFFFFFF0u); }

// ---- 4 ---------------------------------------------------------------------------------------------------------

void test_flapping_never_shortens_gaps() {
    Reconnect r;
    const uint32_t expected_gap[] = {S3, S10, S30, S30, S30};
    uint32_t t = 0;
    EXPECT_ATTEMPT(r, ready(), t);
    r.note_attempt(t, true);
    for (uint32_t gap : expected_gap) {
        t += 5000;  // up for 5 s, then it drops
        r.note_link_lost(t);
        const uint32_t off = first_attempt_offset(r, ready(), t, 2 * S30);
        TEST_ASSERT_EQUAL_INT((int)gap, (int)off);
        t += off;
        r.note_attempt(t, true);
    }
}

void test_flap_after_stable_link_goes_back_to_3s() {
    Reconnect r;
    uint32_t t = 0;
    // drive the step to its last value first
    r.note_connected(t);
    for (int i = 0; i < 4; i++) {
        t += 5000;
        r.note_link_lost(t);
        t += first_attempt_offset(r, ready(), t, 2 * S30);
        r.note_attempt(t, true);
    }
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
    t += STABLE + 7;  // stable link
    r.note_link_lost(t);
    TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
    TEST_ASSERT_EQUAL_INT((int)S3, (int)first_attempt_offset(r, ready(), t, 2 * S30));
}

// ---- 5 ---------------------------------------------------------------------------------------------------------

void test_forgotten_note_falls_back_to_one_attempt_per_interval_step0() {
    Reconnect r;
    EXPECT_ATTEMPT(r, ready(), 100);
    EXPECT_WAIT(r, ready(), 101);
    EXPECT_WAIT(r, ready(), 100 + S3 - 1);
    EXPECT_ATTEMPT(r, ready(), 100 + S3);
    EXPECT_WAIT(r, ready(), 100 + S3 + 1);
    EXPECT_FIRES_AT(r, ready(), 100 + 2 * S3);
    TEST_ASSERT_EQUAL_INT(0, r.backoff_step());  // the step is not advanced by poll
}

void test_forgotten_note_falls_back_to_one_attempt_per_interval_step2() {
    Reconnect r;
    r.note_connected(0);
    r.note_link_lost(10);           // step 1
    r.note_attempt(10, false);      // step 2
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
    const uint32_t due = 10 + S10;
    EXPECT_ATTEMPT(r, ready(), due);
    EXPECT_WAIT(r, ready(), due + 1);
    EXPECT_WAIT(r, ready(), due + S30 - 1);
    EXPECT_ATTEMPT(r, ready(), due + S30);
    EXPECT_FIRES_AT(r, ready(), due + 2 * S30);
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
}

// ---- 6 ---------------------------------------------------------------------------------------------------------

void test_note_attempt_false_replaces_the_safety_deadline() {
    Reconnect r;
    boot_attempt_failed(r, 0);       // step 1, due at 3000
    EXPECT_ATTEMPT(r, ready(), S3);  // safety deadline would be S3 + S10
    r.note_attempt(S3 + 500, false);  // real backoff: S3 + 500 + S10, step 2
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
    EXPECT_WAIT(r, ready(), S3 + S10);
    EXPECT_WAIT(r, ready(), S3 + 500 + S10 - 1);
    EXPECT_ATTEMPT(r, ready(), S3 + 500 + S10);
}

// ---- 7 ---------------------------------------------------------------------------------------------------------

void test_success_reports_stop_further_attempts() {
    {
        Reconnect r;
        EXPECT_ATTEMPT(r, ready(), 0);
        r.note_attempt(0, true);
        EXPECT_WAIT(r, ready(), S3);
        EXPECT_WAIT(r, ready(), 3600000u);
    }
    {
        Reconnect r;
        EXPECT_ATTEMPT(r, ready(), 0);
        r.note_connected(0);
        EXPECT_WAIT(r, ready(), S3);
        EXPECT_WAIT(r, ready(), 3600000u);
    }
}

void test_note_attempt_true_enables_stability_tracking() {
    Reconnect r;
    boot_attempt_failed(r, 0);
    r.note_attempt(S3, false);  // step 2
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
    r.note_attempt(100000u, true);
    r.note_link_lost(100000u + STABLE);
    TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
    EXPECT_FIRES_AT(r, ready(), 100000u + STABLE + S3);
}

// ---- 8 ---------------------------------------------------------------------------------------------------------

void test_user_flag_blocks_attempts_until_changed() {
    Reconnect r;
    r.note_connected(0);
    r.note_user_disconnect();
    TEST_ASSERT_TRUE(r.user_disconnected());
    r.note_link_lost(1000);
    EXPECT_WAIT(r, ready(), 1000 + S3);
    EXPECT_WAIT(r, ready(), 1000 + 3600000u);
    r.note_grinder_changed(1000 + 3600000u);
    TEST_ASSERT_FALSE(r.user_disconnected());
    TEST_ASSERT_EQUAL_INT(0, r.backoff_step());
    EXPECT_ATTEMPT(r, ready(), 1000 + 3600000u);
}

void test_grinder_changed_resets_the_step() {
    Reconnect r;
    boot_attempt_failed(r, 0);
    r.note_attempt(S3, false);
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
    r.note_grinder_changed(S3);
    TEST_ASSERT_EQUAL_INT(0, r.backoff_step());
}

void test_standby_clears_the_user_flag_and_wake_attempts() {
    Reconnect r;
    r.note_connected(0);
    r.note_user_disconnect();
    r.note_link_lost(1000);
    EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), 2000);
    TEST_ASSERT_FALSE(r.user_disconnected());
    EXPECT_ATTEMPT(r, ready(), 2001);
}

// ---- 9 ---------------------------------------------------------------------------------------------------------

void test_standby_waits_even_with_a_due_deadline_and_wake_is_immediate() {
    Reconnect r;  // boot: due at once
    EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), 0);
    EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), S30);
    EXPECT_ATTEMPT(r, ready(), S30 + 1);  // no settle
}

void test_due_deadline_waits_in_standby() {
    Reconnect r;
    r.note_connected(0);
    r.note_link_lost(1000);  // due at 4000
    EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), 1000 + S3);
    EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), 1000 + S3 + 5000);
}

void test_loss_during_standby_then_wake_attempts_at_once_and_keeps_the_step() {
    Reconnect r;
    r.note_connected(0);
    EXPECT_WAIT(r, with_state(GaggimateState::STANDBY), 100);  // link up in standby
    r.note_link_lost(200);
    TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
    EXPECT_ATTEMPT(r, ready(), 300);  // wake; the 3 s deadline is not reached
    TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
}

// ---- 10 --------------------------------------------------------------------------------------------------------

void test_deadline_during_busy_attempts_after_busy_plus_settle() {
    Reconnect r;
    r.note_connected(0);
    r.note_link_lost(1000);  // due at 4000
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 1500);
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 4000);
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 4500);
    EXPECT_WAIT(r, ready(), 5000);  // BUSY ended: settle until 6000
    EXPECT_FIRES_AT(r, ready(), 5000 + SETTLE);
}

void test_new_busy_during_settle_restarts_it() {
    Reconnect r;
    r.note_connected(0);
    r.note_link_lost(1000);
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 4500);
    EXPECT_WAIT(r, ready(), 5000);
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 5500);
    EXPECT_WAIT(r, ready(), 5600);
    EXPECT_WAIT(r, ready(), 5600 + SETTLE - 1);
    EXPECT_ATTEMPT(r, ready(), 5600 + SETTLE);
}

void test_busy_at_boot_attempts_one_second_after_idle() {
    Reconnect r;
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 0);
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 500);
    EXPECT_WAIT(r, ready(), 1000);
    EXPECT_FIRES_AT(r, ready(), 1000 + SETTLE);
}

// ---- 11 --------------------------------------------------------------------------------------------------------

void test_stale_settle_does_not_wrap_into_the_future() {
    Reconnect r;
    EXPECT_WAIT(r, with_state(GaggimateState::BUSY), 0);
    Inputs up = link_up();
    EXPECT_WAIT(r, up, 100);  // BUSY ended with the link up: settle until 1100
    EXPECT_WAIT(r, up, 1000000000u);  // a poll in between clears the stale settle
    r.note_link_lost(2000000000u);
    EXPECT_WAIT(r, ready(), 2000000000u + S3 - 1);
    EXPECT_ATTEMPT(r, ready(), 2000000000u + S3);
}

// ---- 12 --------------------------------------------------------------------------------------------------------

void test_link_up_while_attempt_due_waits_then_backoff_on_drop() {
    Reconnect r;  // boot: due at once, but the link is already up
    EXPECT_WAIT(r, link_up(), 0);
    EXPECT_WAIT(r, link_up(), 500);
    r.note_link_lost(1000);
    EXPECT_WAIT(r, ready(), 1000);
    EXPECT_FIRES_AT(r, ready(), 1000 + S3);
}

// ---- 13 --------------------------------------------------------------------------------------------------------

void test_grinder_changed_during_backoff_wait_attempts_at_once() {
    Reconnect r;
    boot_attempt_failed(r, 0);
    r.note_attempt(S3, false);  // step 2, due at S3 + S10
    EXPECT_WAIT(r, ready(), S3 + 1000);
    r.note_grinder_changed(S3 + 1000);
    TEST_ASSERT_EQUAL_INT(0, r.backoff_step());
    EXPECT_ATTEMPT(r, ready(), S3 + 1000);
}

void test_not_stored_waits_forever_until_grinder_changed() {
    Reconnect r;
    Inputs none = ready();
    none.grinder_stored = false;
    EXPECT_WAIT(r, none, 0);
    EXPECT_WAIT(r, none, 3600000u);
    r.note_grinder_changed(3600000u);
    EXPECT_WAIT(r, none, 3600001u);
    EXPECT_ATTEMPT(r, ready(), 3600002u);
}

// ---- 14 --------------------------------------------------------------------------------------------------------

void test_double_report_advances_the_step_twice() {
    Reconnect r;
    EXPECT_ATTEMPT(r, ready(), 0);
    r.note_attempt(0, false);
    TEST_ASSERT_EQUAL_INT(1, r.backoff_step());
    r.note_link_lost(0);
    TEST_ASSERT_EQUAL_INT(2, r.backoff_step());
    EXPECT_WAIT(r, ready(), S3);
    EXPECT_FIRES_AT(r, ready(), S10);
}

}  // namespace

void run_glink_plugin_reconnect_tests() {
    RUN_TEST(test_fresh_object_attempts_at_once);
    RUN_TEST(test_fresh_object_attempts_at_any_clock_value);
    RUN_TEST(test_single_ineligibility_waits);
    RUN_TEST(test_loss_backoff_sequence);
    RUN_TEST(test_loss_backoff_sequence_across_wrap);
    RUN_TEST(test_stability_threshold);
    RUN_TEST(test_stability_threshold_across_wrap);
    RUN_TEST(test_flapping_never_shortens_gaps);
    RUN_TEST(test_flap_after_stable_link_goes_back_to_3s);
    RUN_TEST(test_forgotten_note_falls_back_to_one_attempt_per_interval_step0);
    RUN_TEST(test_forgotten_note_falls_back_to_one_attempt_per_interval_step2);
    RUN_TEST(test_note_attempt_false_replaces_the_safety_deadline);
    RUN_TEST(test_success_reports_stop_further_attempts);
    RUN_TEST(test_note_attempt_true_enables_stability_tracking);
    RUN_TEST(test_user_flag_blocks_attempts_until_changed);
    RUN_TEST(test_grinder_changed_resets_the_step);
    RUN_TEST(test_standby_clears_the_user_flag_and_wake_attempts);
    RUN_TEST(test_standby_waits_even_with_a_due_deadline_and_wake_is_immediate);
    RUN_TEST(test_due_deadline_waits_in_standby);
    RUN_TEST(test_loss_during_standby_then_wake_attempts_at_once_and_keeps_the_step);
    RUN_TEST(test_deadline_during_busy_attempts_after_busy_plus_settle);
    RUN_TEST(test_new_busy_during_settle_restarts_it);
    RUN_TEST(test_busy_at_boot_attempts_one_second_after_idle);
    RUN_TEST(test_stale_settle_does_not_wrap_into_the_future);
    RUN_TEST(test_link_up_while_attempt_due_waits_then_backoff_on_drop);
    RUN_TEST(test_grinder_changed_during_backoff_wait_attempts_at_once);
    RUN_TEST(test_not_stored_waits_forever_until_grinder_changed);
    RUN_TEST(test_double_report_advances_the_step_twice);
}
