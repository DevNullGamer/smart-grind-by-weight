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
        s.set_wanted(d, true, 150, false, 0);
        TEST_ASSERT_FALSE(s.has_wanted());
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::OUT_OF_RANGE_DOSE);
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);  // once
        TEST_ASSERT_TRUE(poll(s, ok_view(), 0) == SyncAction::NONE);
    }
    const int32_t good[] = {50, 600};
    for (int32_t d : good) {
        SettingsSync s;
        s.set_wanted(d, false, 0, false, 0);
        TEST_ASSERT_TRUE(s.has_wanted());
        ProfileUpdate u;
        TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
        TEST_ASSERT_TRUE(u.has_dose);
        TEST_ASSERT_EQUAL_INT32(d, u.dose_tenths);
    }
}

void test_out_of_range_drops_an_older_unsent_value() {
    SettingsSync s;
    s.set_wanted(180, true, 150, false, 0);
    s.set_wanted(40, true, 150, false, 0);
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
        s.set_wanted(180, c.has, c.size, false, 0);
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
    s.set_wanted(180, true, 150, false, 0);
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
    s.set_wanted(180, true, 150, false, 0);
    SyncView v = ok_view();
    v.has_entry3 = true;
    v.entry_dose_tenths = 180;
    v.entry_has_size = true;
    v.entry_size_tenths = 160;
    TEST_ASSERT_TRUE(poll(s, v, 0) == SyncAction::UPDATE);
    // entry without a size while a size is wanted also differs
    SettingsSync t;
    t.set_wanted(180, true, 150, false, 0);
    v.entry_has_size = false;
    TEST_ASSERT_TRUE(poll(t, v, 0) == SyncAction::UPDATE);
}

void test_compare_first_size_not_wanted_mirrors_a_stale_size() {
    // Q1 mirror (chg026): no size wanted while the cached entry holds one -> a request that clears it, even if the dose matches
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
    SyncView v = ok_view();
    v.has_entry3 = true;
    v.entry_dose_tenths = 180;
    v.entry_has_size = true;
    v.entry_size_tenths = 999;
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, v, 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_dose);
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_TRUE(u.has_size);
    TEST_ASSERT_TRUE(u.size_null);
    TEST_ASSERT_FALSE(u.has_unit);
    TEST_ASSERT_FALSE(u.has_rpm);
    // entry without a size: nothing to clear, dose equal -> no request
    SettingsSync t;
    t.set_wanted(180, false, 0, false, 0);
    v.entry_has_size = false;
    TEST_ASSERT_TRUE(poll(t, v, 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(t.stage() == SyncStage::IDLE);
    // dose differs: update
    SettingsSync w;
    w.set_wanted(190, false, 0, false, 0);
    TEST_ASSERT_TRUE(poll(w, v, 0) == SyncAction::UPDATE);
}

void test_entry3_absent_sends_with_the_dose() {
    SettingsSync s;
    s.set_wanted(180, true, 150, false, 0);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_TRUE(u.has_size);
    TEST_ASSERT_EQUAL_INT32(150, u.size_tenths);
}

// ---- gating ----

void test_blocked_states_and_busy_client_prevent_sending() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 1) == SyncAction::NONE);  // even if the view claims idle
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

// ---- Q1: select after a stored change ----

void test_select_follows_a_changed_update_after_1100_ms() {
    SettingsSync s;
    s.set_wanted(180, true, 150, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), resp - 100) == SyncAction::UPDATE);
    s.on_response(true, "", true, resp);
    TEST_ASSERT_TRUE(poll(s, ok_view(), resp + 1099u) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), resp + 1100u) == SyncAction::SELECT);
}

void test_select_not_sent_when_update_changed_nothing() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.on_response(true, "", false, 100);  // success but nothing stored
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

void test_select_uses_the_same_state_gate() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
    send(s, ok_view(), 0);
    s.on_response(true, "", true, 0);
    s.on_link_down();
    TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
}

// ---- retries ----

void test_rate_limited_retry_not_before_1100_ms() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 5000) == SyncAction::UPDATE);
    s.on_response(false, "RATE_LIMITED", false, 5100);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 5100) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 6199) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), 6200) == SyncAction::UPDATE);
}

void test_rate_limited_retry_across_the_millis_wrap() {
    const uint32_t t = 0xFFFFFFFFu - 300u;
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), t - 100) == SyncAction::UPDATE);
    s.on_response(false, "RATE_LIMITED", false, t);
    TEST_ASSERT_TRUE(poll(s, ok_view(), t + 1099u) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(s, ok_view(), t + 1100u) == SyncAction::UPDATE);
}

void test_wait_codes_wait_for_a_writable_state_then_retry() {
    const char* codes[] = {"NOT_READY", "EDIT_IN_PROGRESS"};
    for (const char* code : codes) {
        SettingsSync s;
        s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
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
        s.set_wanted(180, false, 0, false, 0);
        TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
        s.on_response(false, code, false, 10);
        TEST_ASSERT_TRUE(s.stage() == SyncStage::GAVE_UP);
        TEST_ASSERT_EQUAL_STRING(code, s.gave_up_code());
        TEST_ASSERT_TRUE(poll(s, ok_view(), 100000) == SyncAction::NONE);
    }
}

void test_timeout_counts_as_an_attempt_and_gives_up_after_three() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
    SyncView blocked = ok_view();
    blocked.writes_ok = false;
    uint32_t now = 0;
    for (int i = 0; i < 2; i++) {  // two failed attempts
        send(s, ok_view(), now);
        s.on_response(false, "NOT_READY", false, now + 10);
        poll(s, blocked, now + 20);
        now += 2000;
    }
    s.set_wanted(190, false, 0, false, 0);  // newer value: attempts start again
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
    s.set_wanted(180, true, 150, false, 0);
    s.set_wanted(190, false, 0, false, 0);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(190, u.dose_tenths);
    TEST_ASSERT_FALSE(u.has_size);
}

void test_response_to_an_older_value_is_ignored() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.set_wanted(190, false, 0, false, 0);  // changes while 180 is in flight
    s.on_response(true, "", true, 10);  // answer about 180: must not satisfy 190 nor queue a select
    TEST_ASSERT_TRUE(s.stage() == SyncStage::UPDATE_PENDING);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, ok_view(), 20, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(190, u.dose_tenths);
}

void test_link_drop_keeps_the_wanted_value() {
    SettingsSync s;
    s.set_wanted(180, true, 150, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
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
    s.set_wanted(180, false, 0, false, 0);
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(send(s, ok_view(), 0) == SyncAction::UPDATE);
    s.on_response(true, "", false, 10);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    s.set_wanted(180, false, 0, false, 0);  // after satisfied: queued again (compare-first decides)
    TEST_ASSERT_TRUE(poll(s, ok_view(), 20) == SyncAction::UPDATE);
}

// ---- chg026 step 4: rpm (spec 1.2) and the Q1 mirror ------------------------------------------------------------------------

SyncView rpm_view() {  // a ready grinder that advertises protoMinor >= 2
    SyncView v = ok_view();
    v.rpm_supported = true;
    return v;
}

// cached entry 3 on top of a view
SyncView with_entry(SyncView v, int32_t dose, bool has_size, int32_t size, bool has_rpm, int32_t rpm) {
    v.has_entry3 = true;
    v.entry_dose_tenths = dose;
    v.entry_has_size = has_size;
    v.entry_size_tenths = size;
    v.entry_has_rpm = has_rpm;
    v.entry_rpm = rpm;
    return v;
}

void test_rpm_supported_sends_one_update_carrying_it() {
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(send(s, rpm_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_dose);
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_TRUE(u.has_size);
    TEST_ASSERT_FALSE(u.size_null);
    TEST_ASSERT_EQUAL_INT32(150, u.size_tenths);
    TEST_ASSERT_TRUE(u.has_rpm);
    TEST_ASSERT_FALSE(u.rpm_null);
    TEST_ASSERT_EQUAL_INT32(600, u.rpm);
    TEST_ASSERT_FALSE(u.has_unit);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 1) == SyncAction::NONE);  // one request at a time

    // rpm only (no size): dose and rpm; rpm 0 is a value
    SettingsSync t;
    t.set_wanted(180, false, 0, true, 0);
    ProfileUpdate w;
    TEST_ASSERT_TRUE(poll(t, rpm_view(), 0, &w) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(w.has_size);
    TEST_ASSERT_TRUE(w.has_rpm);
    TEST_ASSERT_EQUAL_INT32(0, w.rpm);
    // the limits 0 and 9999 are valid
    SettingsSync m;
    m.set_wanted(180, false, 0, true, 9999);
    ProfileUpdate x;
    TEST_ASSERT_TRUE(poll(m, rpm_view(), 0, &x) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(9999, x.rpm);
    TEST_ASSERT_TRUE(m.take_note() == SyncNote::NONE);
}

void test_compare_first_includes_the_rpm() {
    // equal in dose, size and rpm: no request and no select
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    const SyncView eq = with_entry(rpm_view(), 180, true, 150, true, 600);
    TEST_ASSERT_TRUE(poll(s, eq, 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, eq, 5000) == SyncAction::NONE);

    // only the rpm differs
    SettingsSync t;
    t.set_wanted(180, true, 150, true, 650);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(t, eq, 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_rpm);
    TEST_ASSERT_EQUAL_INT32(650, u.rpm);
    // the entry has no rpm while one is wanted
    SettingsSync w;
    w.set_wanted(180, true, 150, true, 600);
    TEST_ASSERT_TRUE(poll(w, with_entry(rpm_view(), 180, true, 150, false, 0), 0) == SyncAction::UPDATE);
    // rpm 0 wanted, entry has none: differs (0 is a value)
    SettingsSync z;
    z.set_wanted(180, true, 150, true, 0);
    TEST_ASSERT_TRUE(poll(z, with_entry(rpm_view(), 180, true, 150, false, 0), 0) == SyncAction::UPDATE);
    // the dose or the size differs while the rpm is equal
    SettingsSync d;
    d.set_wanted(190, true, 150, true, 600);
    TEST_ASSERT_TRUE(poll(d, eq, 0) == SyncAction::UPDATE);
    SettingsSync g;
    g.set_wanted(180, true, 160, true, 600);
    TEST_ASSERT_TRUE(poll(g, eq, 0) == SyncAction::UPDATE);
}

void test_rpm_not_supported_is_never_sent_and_noted_once() {
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(send(s, ok_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(u.has_rpm);
    TEST_ASSERT_FALSE(u.rpm_null);
    TEST_ASSERT_TRUE(u.has_dose);
    TEST_ASSERT_TRUE(u.has_size);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::RPM_NOT_SUPPORTED);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);  // once
    s.on_response(true, "", true, 100);                   // changed: select follows, the note does not come again
    TEST_ASSERT_TRUE(poll(s, ok_view(), 1300) == SyncAction::SELECT);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);

    // polling many times without a send does not repeat the note
    SettingsSync t;
    t.set_wanted(180, false, 0, true, 600);
    SyncView blocked = ok_view();
    blocked.writes_ok = false;
    for (int i = 0; i < 5; i++) poll(t, blocked, i);
    TEST_ASSERT_TRUE(t.take_note() == SyncNote::RPM_NOT_SUPPORTED);
    for (int i = 0; i < 5; i++) poll(t, blocked, 10 + i);
    TEST_ASSERT_TRUE(t.take_note() == SyncNote::NONE);

    // the same rpm handed over again does not note again, a different rpm does
    t.set_wanted(180, false, 0, true, 600);
    poll(t, blocked, 20);
    TEST_ASSERT_TRUE(t.take_note() == SyncNote::NONE);
    t.set_wanted(180, false, 0, true, 650);
    poll(t, blocked, 21);
    TEST_ASSERT_TRUE(t.take_note() == SyncNote::RPM_NOT_SUPPORTED);

    // a new session (link down) may be another grinder: noted again
    t.on_link_down();
    poll(t, blocked, 30);
    TEST_ASSERT_TRUE(t.take_note() == SyncNote::RPM_NOT_SUPPORTED);

    // not before the link is ready, and never when no rpm is wanted
    SettingsSync n;
    n.set_wanted(180, false, 0, true, 600);
    SyncView down = ok_view();
    down.ready = false;
    poll(n, down, 0);
    TEST_ASSERT_TRUE(n.take_note() == SyncNote::NONE);
    SettingsSync p;
    p.set_wanted(180, false, 0, false, 0);
    poll(p, ok_view(), 0);
    TEST_ASSERT_TRUE(p.take_note() == SyncNote::NONE);

    // an old grinder with an equal dose/size entry: satisfied without a request, no rpm member ever
    SettingsSync q;
    q.set_wanted(180, true, 150, true, 600);
    TEST_ASSERT_TRUE(poll(q, with_entry(ok_view(), 180, true, 150, false, 0), 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(q.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(q.take_note() == SyncNote::RPM_NOT_SUPPORTED);
}

void test_old_grinder_session_replays_the_chg025_behaviour() {
    // A grinder without protoMinor: wanted dose and size only (no rpm on either side). Same bytes, same select, same gates.
    SettingsSync s;
    s.set_wanted(180, true, 150, false, 0);
    const SyncView old_view = with_entry(ok_view(), 170, true, 140, false, 0);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(send(s, old_view, 1000, &u) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(u.has_rpm);
    TEST_ASSERT_FALSE(u.rpm_null);
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_EQUAL_INT32(150, u.size_tenths);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);  // no rpm wanted: no note at all
    s.on_response(true, "", true, 2000);
    TEST_ASSERT_TRUE(poll(s, old_view, 3099) == SyncAction::NONE);
    TEST_ASSERT_TRUE(send(s, old_view, 3100) == SyncAction::SELECT);
    s.on_response(true, "", false, 3200);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);

    // an old grinder never gets an rpm:null either, even if the (stale) view says the entry has one
    SettingsSync t;
    t.set_wanted(180, false, 0, false, 0);
    ProfileUpdate w;
    TEST_ASSERT_TRUE(poll(t, with_entry(ok_view(), 180, false, 0, true, 600), 0) == SyncAction::NONE);
    SettingsSync x;
    x.set_wanted(190, false, 0, false, 0);
    TEST_ASSERT_TRUE(poll(x, with_entry(ok_view(), 180, false, 0, true, 600), 0, &w) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(w.has_rpm);
}

void test_rpm_out_of_range_is_noted_and_treated_as_absent() {
    const int32_t bad[] = {10000, -1, 99999};
    for (int32_t rpm : bad) {
        SettingsSync s;
        s.set_wanted(180, true, 150, true, rpm);
        TEST_ASSERT_TRUE(s.has_wanted());  // the dose and size still go out
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::RPM_OUT_OF_RANGE);
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);
        ProfileUpdate u;
        TEST_ASSERT_TRUE(poll(s, rpm_view(), 0, &u) == SyncAction::UPDATE);
        TEST_ASSERT_FALSE(u.has_rpm);  // treated as absent: entry has no rpm, nothing to clear
        TEST_ASSERT_TRUE(u.has_dose);
        TEST_ASSERT_TRUE(u.has_size);
        TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);  // and no unsupported note on top
    }
    // absent also means mirror: the entry holds an rpm -> cleared
    SettingsSync t;
    t.set_wanted(180, true, 150, true, 10000);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(t, with_entry(rpm_view(), 180, true, 150, true, 600), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_rpm);
    TEST_ASSERT_TRUE(u.rpm_null);
    // on an old grinder the out-of-range note is the one raised (the value is absent, so no unsupported note)
    SettingsSync o;
    o.set_wanted(180, false, 0, true, 10000);
    TEST_ASSERT_TRUE(o.take_note() == SyncNote::RPM_OUT_OF_RANGE);
    ProfileUpdate ou;
    TEST_ASSERT_TRUE(poll(o, ok_view(), 0, &ou) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(ou.has_rpm);
    TEST_ASSERT_TRUE(o.take_note() == SyncNote::NONE);
    // a bad dose wins: only the dose note, nothing queued
    SettingsSync d;
    d.set_wanted(49, false, 0, true, 10000);
    TEST_ASSERT_FALSE(d.has_wanted());
    TEST_ASSERT_TRUE(d.take_note() == SyncNote::OUT_OF_RANGE_DOSE);
    TEST_ASSERT_TRUE(d.take_note() == SyncNote::NONE);
}

void test_rpm_out_of_range_note_is_raised_again_for_the_same_value_pinned() {
    // Pins today's behaviour (Auditor note, non-blocking): the same invalid rpm handed over again raises the note again;
    // GrinderCore::set_wanted drops an unchanged value first, and re-hands it over on every ready (see the core test).
    SettingsSync s;
    s.set_wanted(180, false, 0, true, 10000);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::RPM_OUT_OF_RANGE);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 0) == SyncAction::UPDATE);  // pending, not yet sent
    s.set_wanted(180, false, 0, true, 10000);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::RPM_OUT_OF_RANGE);
    TEST_ASSERT_TRUE(s.take_note() == SyncNote::NONE);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::UPDATE_PENDING);  // the repeat did not disturb the pending update
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 1, &u) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(u.has_rpm);
}

void test_mirror_clears_a_stale_size_and_rpm_in_one_update() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);  // wanted has neither
    ProfileUpdate u;
    TEST_ASSERT_TRUE(send(s, with_entry(rpm_view(), 180, true, 145, true, 600), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_dose);
    TEST_ASSERT_EQUAL_INT32(180, u.dose_tenths);
    TEST_ASSERT_TRUE(u.has_size);
    TEST_ASSERT_TRUE(u.size_null);
    TEST_ASSERT_TRUE(u.has_rpm);
    TEST_ASSERT_TRUE(u.rpm_null);
    TEST_ASSERT_FALSE(u.has_unit);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 1) == SyncAction::NONE);  // a single update
    // the select follows the stored change as usual
    s.on_response(true, "", true, 100);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 1199) == SyncAction::NONE);
    TEST_ASSERT_TRUE(send(s, rpm_view(), 1200) == SyncAction::SELECT);
}

void test_mirror_clears_only_what_the_entry_holds() {
    // entry has size only
    SettingsSync a;
    a.set_wanted(180, false, 0, false, 0);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(a, with_entry(rpm_view(), 180, true, 145, false, 0), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_size && u.size_null);
    TEST_ASSERT_FALSE(u.has_rpm);
    // entry has rpm only
    SettingsSync b;
    b.set_wanted(180, false, 0, false, 0);
    ProfileUpdate v;
    TEST_ASSERT_TRUE(poll(b, with_entry(rpm_view(), 180, false, 0, true, 600), 0, &v) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(v.has_size);
    TEST_ASSERT_TRUE(v.has_rpm && v.rpm_null);
    // entry has none: nothing to clear, dose equal -> no request
    SettingsSync c;
    c.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(poll(c, with_entry(rpm_view(), 180, false, 0, false, 0), 0) == SyncAction::NONE);
    TEST_ASSERT_TRUE(c.stage() == SyncStage::IDLE);
    // entry has none, dose differs -> dose only, no null members
    SettingsSync d;
    d.set_wanted(190, false, 0, false, 0);
    ProfileUpdate w;
    TEST_ASSERT_TRUE(poll(d, with_entry(rpm_view(), 180, false, 0, false, 0), 0, &w) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(w.has_dose);
    TEST_ASSERT_FALSE(w.has_size);
    TEST_ASSERT_FALSE(w.has_rpm);
    // no entry 3: create with what is wanted only (dose only here), no clears
    SettingsSync e;
    e.set_wanted(180, false, 0, false, 0);
    ProfileUpdate x;
    TEST_ASSERT_TRUE(poll(e, rpm_view(), 0, &x) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(x.has_dose);
    TEST_ASSERT_FALSE(x.has_size);
    TEST_ASSERT_FALSE(x.size_null);
    TEST_ASSERT_FALSE(x.has_rpm);
    TEST_ASSERT_FALSE(x.rpm_null);
    // size wanted, no rpm wanted, entry has an rpm: the size is sent, the rpm cleared
    SettingsSync f;
    f.set_wanted(180, true, 145, false, 0);
    ProfileUpdate y;
    TEST_ASSERT_TRUE(poll(f, with_entry(rpm_view(), 180, true, 145, true, 600), 0, &y) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(y.has_size && !y.size_null);
    TEST_ASSERT_TRUE(y.has_rpm && y.rpm_null);
    // rpm wanted, no size wanted, entry has a size: the rpm is sent, the size cleared
    SettingsSync g;
    g.set_wanted(180, false, 0, true, 600);
    ProfileUpdate z;
    TEST_ASSERT_TRUE(poll(g, with_entry(rpm_view(), 180, true, 145, true, 600), 0, &z) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(z.has_size && z.size_null);
    TEST_ASSERT_TRUE(z.has_rpm && !z.rpm_null);
    TEST_ASSERT_EQUAL_INT32(600, z.rpm);
    // an old grinder: the size is still mirrored, the rpm is not touched
    SettingsSync h;
    h.set_wanted(180, false, 0, false, 0);
    ProfileUpdate o;
    TEST_ASSERT_TRUE(poll(h, with_entry(ok_view(), 180, true, 145, true, 600), 0, &o) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(o.has_size && o.size_null);
    TEST_ASSERT_FALSE(o.has_rpm);
}

void test_mirror_satisfied_after_an_unchanged_answer_and_no_select() {
    SettingsSync s;
    s.set_wanted(180, false, 0, false, 0);
    const SyncView stale = with_entry(rpm_view(), 180, true, 145, true, 600);
    TEST_ASSERT_TRUE(send(s, stale, 0) == SyncAction::UPDATE);
    s.on_response(true, "", false, 100);  // the grinder already held it
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, stale, 100000) == SyncAction::NONE);  // no select
    // a refresh then shows the cleared entry: nothing more to send
    s.set_wanted(180, false, 0, false, 0);
    TEST_ASSERT_TRUE(poll(s, with_entry(rpm_view(), 180, false, 0, false, 0), 100001) == SyncAction::NONE);
}

void test_select_after_a_changed_update_with_rpm() {
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    TEST_ASSERT_TRUE(send(s, rpm_view(), 1000) == SyncAction::UPDATE);
    s.on_response(true, "", true, 2000);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::SELECT_PENDING);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 3099) == SyncAction::NONE);
    TEST_ASSERT_TRUE(send(s, rpm_view(), 3100) == SyncAction::SELECT);
    s.on_response(true, "", false, 3200);
    TEST_ASSERT_TRUE(s.stage() == SyncStage::IDLE);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 100000) == SyncAction::NONE);  // exactly once
    // unchanged answer: no select
    SettingsSync t;
    t.set_wanted(180, true, 150, true, 600);
    send(t, rpm_view(), 0);
    t.on_response(true, "", false, 10);
    TEST_ASSERT_TRUE(poll(t, rpm_view(), 100000) == SyncAction::NONE);
}

void test_rate_limit_and_wait_codes_with_rpm() {
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    ProfileUpdate u;
    TEST_ASSERT_TRUE(send(s, rpm_view(), 5000, &u) == SyncAction::UPDATE);
    s.on_response(false, "RATE_LIMITED", false, 5100);
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 6199) == SyncAction::NONE);
    ProfileUpdate again;
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 6200, &again) == SyncAction::UPDATE);  // 1.1 s after the answer
    TEST_ASSERT_TRUE(again.has_rpm);
    TEST_ASSERT_EQUAL_INT32(600, again.rpm);

    // NOT_READY waits for a writable state, then the same rpm goes out
    SettingsSync t;
    t.set_wanted(180, false, 0, true, 650);
    TEST_ASSERT_TRUE(send(t, rpm_view(), 0) == SyncAction::UPDATE);
    t.on_response(false, "NOT_READY", false, 10);
    SyncView blocked = rpm_view();
    blocked.writes_ok = false;
    TEST_ASSERT_TRUE(poll(t, blocked, 20) == SyncAction::NONE);
    TEST_ASSERT_TRUE(poll(t, blocked, 2000) == SyncAction::NONE);
    ProfileUpdate w;
    TEST_ASSERT_TRUE(send(t, rpm_view(), 2100, &w) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(650, w.rpm);
}

void test_latest_wins_when_only_the_rpm_changes() {
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    s.set_wanted(180, true, 150, true, 650);  // unsent: replaced
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 0, &u) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(650, u.rpm);

    // an in-flight request about the older rpm: its answer is ignored, the newer one goes out
    SettingsSync t;
    t.set_wanted(180, true, 150, true, 600);
    TEST_ASSERT_TRUE(send(t, rpm_view(), 0) == SyncAction::UPDATE);
    t.set_wanted(180, true, 150, true, 650);
    t.on_response(true, "", true, 10);  // about 600: must not queue a select or satisfy 650
    TEST_ASSERT_TRUE(t.stage() == SyncStage::UPDATE_PENDING);
    ProfileUpdate w;
    TEST_ASSERT_TRUE(poll(t, rpm_view(), 20, &w) == SyncAction::UPDATE);
    TEST_ASSERT_EQUAL_INT32(650, w.rpm);

    // dropping the rpm is a change too (rpm -> none)
    SettingsSync d;
    d.set_wanted(180, true, 150, true, 600);
    d.set_wanted(180, true, 150, false, 0);
    ProfileUpdate x;
    TEST_ASSERT_TRUE(poll(d, with_entry(rpm_view(), 180, true, 150, true, 600), 0, &x) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(x.has_rpm && x.rpm_null);

    // the same rpm again while pending is ignored (attempts are kept), a different rpm resets them
    SettingsSync a;
    a.set_wanted(180, false, 0, true, 600);
    uint32_t now = 0;
    SyncView blocked = rpm_view();
    blocked.writes_ok = false;
    for (int i = 0; i < 2; i++) {
        send(a, rpm_view(), now);
        a.on_response(false, "NOT_READY", false, now + 10);
        poll(a, blocked, now + 20);
        now += 2000;
    }
    a.set_wanted(180, false, 0, true, 700);  // newer value: attempts start again
    for (int i = 0; i < 2; i++) {
        ProfileUpdate y;
        TEST_ASSERT_TRUE(send(a, rpm_view(), now, &y) == SyncAction::UPDATE);
        TEST_ASSERT_EQUAL_INT32(700, y.rpm);
        a.on_response(false, "NOT_READY", false, now + 10);
        TEST_ASSERT_TRUE(a.stage() == SyncStage::UPDATE_PENDING);
        poll(a, blocked, now + 20);
        now += 2000;
    }
}

void test_link_drop_keeps_the_wanted_rpm() {
    SettingsSync s;
    s.set_wanted(180, true, 150, true, 600);
    TEST_ASSERT_TRUE(send(s, rpm_view(), 0) == SyncAction::UPDATE);
    s.on_link_down();
    TEST_ASSERT_TRUE(s.has_wanted());
    ProfileUpdate u;
    TEST_ASSERT_TRUE(poll(s, rpm_view(), 10, &u) == SyncAction::UPDATE);
    TEST_ASSERT_TRUE(u.has_rpm);
    TEST_ASSERT_EQUAL_INT32(600, u.rpm);
    // the next link is an old grinder: the same wanted value goes out without the rpm
    SettingsSync t;
    t.set_wanted(180, true, 150, true, 600);
    send(t, rpm_view(), 0);
    t.on_link_down();
    ProfileUpdate o;
    TEST_ASSERT_TRUE(poll(t, ok_view(), 10, &o) == SyncAction::UPDATE);
    TEST_ASSERT_FALSE(o.has_rpm);
    TEST_ASSERT_TRUE(t.take_note() == SyncNote::RPM_NOT_SUPPORTED);
}

}  // namespace

void run_glink_plugin_settingssync_tests() {
    RUN_TEST(test_dose_range);
    RUN_TEST(test_out_of_range_drops_an_older_unsent_value);
    RUN_TEST(test_size_rules_and_no_unit);
    RUN_TEST(test_compare_first_equal_sends_nothing_and_no_select);
    RUN_TEST(test_compare_first_dose_equal_size_differs_sends);
    RUN_TEST(test_compare_first_size_not_wanted_mirrors_a_stale_size);
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
    RUN_TEST(test_rpm_supported_sends_one_update_carrying_it);
    RUN_TEST(test_compare_first_includes_the_rpm);
    RUN_TEST(test_rpm_not_supported_is_never_sent_and_noted_once);
    RUN_TEST(test_old_grinder_session_replays_the_chg025_behaviour);
    RUN_TEST(test_rpm_out_of_range_is_noted_and_treated_as_absent);
    RUN_TEST(test_rpm_out_of_range_note_is_raised_again_for_the_same_value_pinned);
    RUN_TEST(test_mirror_clears_a_stale_size_and_rpm_in_one_update);
    RUN_TEST(test_mirror_clears_only_what_the_entry_holds);
    RUN_TEST(test_mirror_satisfied_after_an_unchanged_answer_and_no_select);
    RUN_TEST(test_select_after_a_changed_update_with_rpm);
    RUN_TEST(test_rate_limit_and_wait_codes_with_rpm);
    RUN_TEST(test_latest_wins_when_only_the_rpm_changes);
    RUN_TEST(test_link_drop_keeps_the_wanted_rpm);
}
