// GLinkSettingsSync (chg025 3.4 and section 0 Q1/Q2): when `grindProfile update` of profile 3 and the one follow-up
// `grindProfile select 3` go out. Pure, injected clock.
#include <stdint.h>
#include <unity.h>
#include "GLinkSettingsSync.h"

using namespace glinkc;

namespace {

SyncView ok_view() {
    SyncView v;
    v.ready = true;
    v.client_idle = true;
    v.writes_ok = true;
    return v;
}

SyncAction poll(SettingsSync& s, const SyncView& v, uint32_t now, ProfileUpdate* u = nullptr) {
    ProfileUpdate tmp;
    return s.next(v, now, u ? u : &tmp);
}

// next() and, when something goes out, on_sent().
SyncAction send(SettingsSync& s, const SyncView& v, uint32_t now, ProfileUpdate* u = nullptr) {
    const SyncAction a = poll(s, v, now, u);
    if (a != SyncAction::NONE) s.on_sent(now);
    return a;
}

// ---- range and size rules ----

void test_dose_range() {
    const int32_t bad[] = {49, 601, 0, -10, 1000};  // 4.9, 60.1, 0, -1, 100
    for (int32_t d : bad) {
        SettingsSync s;
        s.set_wanted(d, true, 150);
        TEST_ASSERT_FALSE(s.has_wanted());
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::OUT_OF_RANGE_DOSE);
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);  // once
        TEST_ASSERT_TRUE(poll(s, ok_view(), 0) == SyncAction::NONE);
    }
    const int32_t good[] = {50, 600};
    for (int32_t d : good) {
        SettingsSync s;
        s.set_wanted(d, false, 0);
        TEST_ASSERT_TRUE(s.has_wanted());
        ProfileUpdate u;
        TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
        TEST_ASSERT_TRUE(u.has_dose);
        TEST_ASSERT_EQUAL_INT32(d, u.dose_tenths);
    }
}

void test_out_of_range_drops_an_older_unsent_value() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    s.set_wanted(40, true, 150);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 0) == SyncAction::NONE);
}

void test_size_rules_and_no_unit() {
    struct Case { bool has; int32_t size; bool expect_size; int32_t expect; };
    const Case cases[] = {
        {true, 0, true, 0},          // 0.0 valid
        {true, 9999, true, 9999},    // 999.9 valid
        {true, 10000, false, 0},     // 1000.0 -> dose only
        {true, -1, false, 0},        // negative -> dose only
        {false, 0, false, 0},        // unset -> dose only
    };
    for (const Case& c : cases) {
        SettingsSync s;
        s.set_wanted(180, c.has, c.size);
        TEST_ASSERT_TRUE(s.has_wanted());  // dose is still queued
        ProfileUpdate u;
        TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
        TEST_ASSERT_TRUE(u.has_dose);
        TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
        TEST_ASSERT_TRUE(c.expect_size == u.has_size);
        if (c.expect_size) TEST_ASSERT_EQUAL_INT32(c.expect, u.size_tenths);
        TEST_ASSERT_FALSE(u.size_null);
        TEST_ASSERT_FALSE(u.has_unit);  // Q2: never a grindUnit
        TEST_ASSERT_FALSE(u.unit_null);
    }
}

// ---- compare-first ----

void test_compare_first_equal_sends_nothing_and_no_select() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    SyncView v = ok_view();
    v.has_entry3 = true;
    v.entry_dose_tenths = 180;
    v.entry_has_size = true;
    v.entry_size_tenths = 150;
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, v, 5000) == SyncAction::NONE);  // never a select afterwards
    TEST_ASSERT_TRUE(poll(s, ok_view(), 6000) == SyncAction::NONE);
}

void test_compare_first_dose_equal_size_differs_sends() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    SyncView v = ok_view();
    v.has_entry3 = true;
    v.entry_dose_tenths = 180;
    v.entry_has_size = true;
    v.entry_size_tenths = 160;
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::UPDATE);
    // entry without a size while a size is wanted also differs
    SettingsSync t;
    t.set_wanted(180, true, 150);
    v.entry_has_size = false;
    TEST_ASSERT_TRUE(poll(t, v, 0) == SyncAction::UPDATE);
}

void test_compare_first_size_not_wanted_compares_dose_only() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    SyncView v = ok_view();
    v.has_entry3 = true;
    v.entry_dose_tenths = 180;
    v.entry_has_size = true;
    v.entry_size_tenths = 999;
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    SettingsSync t;
    t.set_wanted(190, false, 0);
    TEST_ASSERT_TRUE(poll(t, v, 0) == SyncAction::UPDATE);  // dose differs
}

void test_entry3_absent_sends_with_the_dose() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_TRUE(u.has_size);
    TEST_ASSERT_EQUAL_INT32(150, u.size_tenths);
}

// ---- gating ----

void test_blocked_states_and_busy_client_prevent_sending() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    SyncView v = ok_view();
    v.writes_ok = false;  // SETTING, GRINDING, UPDATING all map to false
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::NONE);
    v = ok_view();
    v.client_idle = false;
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::NONE);
    v = ok_view();
    v.ready = false;
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 0) == SyncAction::UPDATE);
}

void test_never_two_requests_while_one_is_in_flight() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 1) == SyncAction::NONE);  // even if the view claims idle
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

// ---- Q1: select after a stored change ----

void test_select_follows_a_changed_update_after_1100_ms() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    TEST_ASSERT_TRUE(send(s, ok_view(), 1000) == SyncAction::UPDATE);
    s.on_response(true, "", true, 2000);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::SELECT_PENDING);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 2000) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 3099) == SyncAction::NONE);
    TEST_ASSERT_TRUE(send(s, ok_view(), 3100) == SyncAction::SELECT);
    s.on_response(true, "", false, 3200);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);  // select exactly once
}

void test_select_not_before_response_plus_1100_across_the_wrap() {
    const uint32_t resp = 0xFFFFFFFFu - 500u;  // response shortly before the wrap
    SettingsSync s;
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), resp - 100) == SyncAction::UPDATE);
    s.on_response(true, "", true, resp);
    TEST_ASSERT_TRUE(poll(s, ok_view(), resp + 1099u) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), resp + 1100u) == SyncAction::SELECT);
}

void test_select_not_sent_when_update_changed_nothing() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.on_response(true, "", false, 100);  // success but nothing stored
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

void test_select_uses_the_same_state_gate() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    send(s, ok_view(), 0);
    s.on_response(true, "", true, 10);
    SyncView v = ok_view();
    v.writes_ok = false;
    TEST_ASSERT_TRUE(poll(s, v, 5000) == SyncAction::NONE);
    v.writes_ok = true;
    v.client_idle = false;
    TEST_ASSERT_TRUE(poll(s, v, 5000) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 5000) == SyncAction::SELECT);
}

void test_select_gives_up_after_three_attempts() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    send(s, ok_view(), 0);
    s.on_response(true, "", true, 0);
    uint32_t now = 1100;
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(s.stage() == SyncStage::SELECT_PENDING);
        TEST_ASSERT_TRUE(send(s, ok_view(), now) == SyncAction::SELECT);
        s.on_response(false, "RATE_LIMITED", false, now);
        now += 1100;
    }
    TEST_ASSERT_TRUE(s.stage() == SyncStage::GAVE_UP);
    TEST_ASSERT_TRUE(poll(s, ok_view(), now + 100000) == SyncAction::NONE);
}

void test_select_hard_error_gives_up_at_once() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    send(s, ok_view(), 0);
    s.on_response(true, "", true, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 1100) == SyncAction::SELECT);
    s.on_response(false, "PROFILE_NOT_FOUND", false, 1200);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::GAVE_UP);
    TEST_ASSERT_EQUAL_STRING("PROFILE_NOT_FOUND", s.gave_up_code());
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

void test_select_timeout_is_retried() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    send(s, ok_view(), 0);
    s.on_response(true, "", true, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 1100) == SyncAction::SELECT);
    s.on_timeout(6100);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::SELECT_PENDING);
    // eventually sent again (state wait or 5 s fallback)
    TEST_ASSERT_TRUE(poll(s, ok_view(), 6100 + 5000) == SyncAction::SELECT);
}

void test_link_drop_drops_a_pending_select() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    send(s, ok_view(), 0);
    s.on_response(true, "", true, 0);
    s.on_link_down();
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

// ---- retries ----

void test_rate_limited_retry_not_before_1100_ms() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 5000) == SyncAction::UPDATE);
    s.on_response(false, "RATE_LIMITED", false, 5100);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 5100) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 6199) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 6200) == SyncAction::UPDATE);
}

void test_rate_limited_retry_across_the_millis_wrap() {
    const uint32_t t = 0xFFFFFFFFu - 300u;
    SettingsSync s;
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), t - 100) == SyncAction::UPDATE);
    s.on_response(false, "RATE_LIMITED", false, t);
    TEST_ASSERT_TRUE(poll(s, ok_view(), t + 1099u) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), t + 1100u) == SyncAction::UPDATE);
}

void test_wait_codes_wait_for_a_writable_state_then_retry() {
    const char* codes[] = {"NOT_READY", "EDIT_IN_PROGRESS"};
    for (const char* code : codes) {
        SettingsSync s;
        s.set_wanted(180, false, 0);
        TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
        s.on_response(false, code, false, 10);
        SyncView blocked = ok_view();
        blocked.writes_ok = false;  // SETTING
        TEST_ASSERT_TRUE(poll(s, blocked, 20) == SyncAction::NONE);
        TEST_ASSERT_TRUE(poll(s, blocked, 2000) == SyncAction::NONE);
        TEST_ASSERT_TRUE(send(s, ok_view(), 2100) == SyncAction::UPDATE);  // IDLE again
    }
}

void test_wait_code_three_attempts_then_give_up() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    SyncView blocked = ok_view();
    blocked.writes_ok = false;
    uint32_t now = 0;
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(s.stage() == SyncStage::UPDATE_PENDING);
        TEST_ASSERT_TRUE(send(s, ok_view(), now) == SyncAction::UPDATE);
        s.on_response(false, "NOT_READY", false, now + 10);
        poll(s, blocked, now + 20);
        now += 2000;
    }
    TEST_ASSERT_TRUE(s.stage() == SyncStage::GAVE_UP);
    TEST_ASSERT_EQUAL_STRING("NOT_READY", s.gave_up_code());
    TEST_ASSERT_TRUE(poll(s, ok_view(), now + 100000) == SyncAction::NONE);
}

void test_other_errors_give_up_after_one() {
    const char* codes[] = {"OUT_OF_RANGE", "INVALID_PARAM", "STORE_FAILED", "PROFILE_READ_ONLY"};
    for (const char* code : codes) {
        SettingsSync s;
        s.set_wanted(180, false, 0);
        TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
        s.on_response(false, code, false, 10);
        TEST_ASSERT_TRUE(s.stage() == SyncStage::GAVE_UP);
        TEST_ASSERT_EQUAL_STRING(code, s.gave_up_code());
        TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
    }
}

void test_timeout_counts_as_an_attempt_and_gives_up_after_three() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    uint32_t now = 0;
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(send(s, ok_view(), now) == SyncAction::UPDATE);
        s.on_timeout(now + 5000);
        now += 10000;  // beyond any wait fallback
    }
    TEST_ASSERT_TRUE(s.stage() == SyncStage::GAVE_UP);
}

// ---- wanted value handling ----

void test_latest_wins_and_resets_attempts() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    SyncView blocked = ok_view();
    blocked.writes_ok = false;
    uint32_t now = 0;
    for (int i = 0; i < 2; i++) {  // two failed attempts
        send(s, ok_view(), now);
        s.on_response(false, "NOT_READY", false, now + 10);
        poll(s, blocked, now + 20);
        now += 2000;
    }
    s.set_wanted(190, false, 0);  // newer value: attempts start again
    for (int i = 0; i < 2; i++) {
        ProfileUpdate u;
        TEST_ASSERT_TRUE(send(s, ok_view(), now, &u) == SyncAction::UPDATE);
        TEST_ASSERT_EQUAL_INT32(190, u.dose_tenths);
        s.on_response(false, "NOT_READY", false, now + 10);
        TEST_ASSERT_TRUE(s.stage() == SyncStage::UPDATE_PENDING);  // not given up (would be after 3 old attempts)
        poll(s, blocked, now + 20);
        now += 2000;
    }
}

void test_newer_value_replaces_an_unsent_one() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    s.set_wanted(190, false, 0);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(190, u.dose_tenths);
    TEST_ASSERT_FALSE(u.has_size);
}

void test_response_to_an_older_value_is_ignored() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.set_wanted(190, false, 0);  // changes while 180 is in flight
    s.on_response(true, "", true, 10);  // answer about 180: must not satisfy 190 nor queue a select
    TEST_ASSERT_TRUE(s.stage() == SyncStage::UPDATE_PENDING);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 20, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(190, u.dose_tenths);
}

void test_link_drop_keeps_the_wanted_value() {
    SettingsSync s;
    s.set_wanted(180, true, 150);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.on_link_down();
    TEST_ASSERT_TRUE(s.has_wanted());
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 10, &u) == SyncAction::UPDATE);  // nothing replayed, re-derived
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_EQUAL_INT32(150, u.size_tenths);
}

void test_link_drop_clears_the_attempt_counter() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    SyncView blocked = ok_view();
    blocked.writes_ok = false;
    uint32_t now = 0;
    for (int i = 0; i < 2; i++) {
        send(s, ok_view(), now);
        s.on_response(false, "NOT_READY", false, now + 10);
        poll(s, blocked, now + 20);
        now += 2000;
    }
    s.on_link_down();
    for (int i = 0; i < 2; i++) {  // would give up on the 1st if the counter had been kept
        TEST_ASSERT_TRUE(send(s, ok_view(), now) == SyncAction::UPDATE);
        s.on_response(false, "NOT_READY", false, now + 10);
        TEST_ASSERT_TRUE(s.stage() == SyncStage::UPDATE_PENDING);
        poll(s, blocked, now + 20);
        now += 2000;
    }
}

void test_same_value_again_while_pending_is_ignored_and_after_satisfied_requeued() {
    SettingsSync s;
    s.set_wanted(180, false, 0);
    s.set_wanted(180, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.on_response(true, "", false, 10);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    s.set_wanted(180, false, 0);  // after satisfied: queued again (compare-first decides)
    TEST_ASSERT_TRUE(poll(s, ok_view(), 20) == SyncAction::UPDATE);
}

}  // namespace

void run_glink_plugin_settingssync_tests() {
    RUN_TEST(test_dose_range);
    RUN_TEST(test_out_of_range_drops_an_older_unsent_value);
    RUN_TEST(test_size_rules_and_no_unit);
    RUN_TEST(test_compare_first_equal_sends_nothing_and_no_select);
    RUN_TEST(test_compare_first_dose_equal_size_differs_sends);
    RUN_TEST(test_compare_first_size_not_wanted_compares_dose_only);
    RUN_TEST(test_entry3_absent_sends_with_the_dose);
    RUN_TEST(test_blocked_states_and_busy_client_prevent_sending);
    RUN_TEST(test_never_two_requests_while_one_is_in_flight);
    RUN_TEST(test_select_follows_a_changed_update_after_1100_ms);
    RUN_TEST(test_select_not_before_response_plus_1100_across_the_wrap);
    RUN_TEST(test_select_not_sent_when_update_changed_nothing);
    RUN_TEST(test_select_uses_the_same_state_gate);
    RUN_TEST(test_select_gives_up_after_three_attempts);
    RUN_TEST(test_select_hard_error_gives_up_at_once);
    RUN_TEST(test_select_timeout_is_retried);
    RUN_TEST(test_link_drop_drops_a_pending_select);
    RUN_TEST(test_rate_limited_retry_not_before_1100_ms);
    RUN_TEST(test_rate_limited_retry_across_the_millis_wrap);
    RUN_TEST(test_wait_codes_wait_for_a_writable_state_then_retry);
    RUN_TEST(test_wait_code_three_attempts_then_give_up);
    RUN_TEST(test_other_errors_give_up_after_one);
    RUN_TEST(test_timeout_counts_as_an_attempt_and_gives_up_after_three);
    RUN_TEST(test_latest_wins_and_resets_attempts);
    RUN_TEST(test_newer_value_replaces_an_unsent_one);
    RUN_TEST(test_response_to_an_older_value_is_ignored);
    RUN_TEST(test_link_drop_keeps_the_wanted_value);
    RUN_TEST(test_link_drop_clears_the_attempt_counter);
    RUN_TEST(test_same_value_again_while_pending_is_ignored_and_after_satisfied_requeued);
}
