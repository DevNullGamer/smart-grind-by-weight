#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "bluetooth/glink_decide.h"
#include "bluetooth/glink_mailbox.h"
#include "bluetooth/glink_screen.h"
#include "bluetooth/glink_snapshot.h"

// Host tests of the GLink hand-over to the UI task (chg020 step 6a): the one-request mailbox, the snapshot freshness and
// the get served from it, the screen classification, the periodInfo mapping and the write rate limit driven by a fake clock.

namespace {

constexpr uint32_t kPickup = 1000;
constexpr uint32_t kRun = 2000;

GlinkRequest update_request() {
    GlinkRequest r;
    r.object = GlinkObject::GRIND_PROFILE;
    r.op = GlinkOp::UPDATE;
    r.index = 3;
    r.has_dose = true;
    r.dose_tenths = 180;
    return r;
}

GlinkMailboxResult success_result(uint8_t select) {
    GlinkMailboxResult r;
    r.kind = GlinkReplyKind::SELECT;
    r.select_profile = select;
    return r;
}

GlinkSnapshot make_snapshot(GlinkScreen screen, bool profile3) {
    GlinkSnapshot s;
    s.screen = screen;
    s.profile3_exists = profile3;
    for (int i = 0; i < GLINK_PROFILE_COUNT_MAX; i++) {
        s.profiles[i].index = static_cast<uint8_t>(i);
        s.profiles[i].dose_tenths = 100 + i;
    }
    if (profile3) {
        s.profiles[3].dose_tenths = 180;
        s.profiles[3].has_grind_size = true;
        s.profiles[3].grind_size_tenths = 145;
        strcpy(s.profiles[3].grind_unit, "um");
    }
    return s;
}

}  // namespace

// ---- mailbox: one in flight ----
static void test_mailbox_second_submit_is_refused_until_collected() {
    GlinkMailbox box;
    TEST_ASSERT_TRUE(glink_mailbox_submit(&box, update_request(), 100));
    TEST_ASSERT_FALSE(glink_mailbox_submit(&box, update_request(), 101));  // QUEUED: BUSY for the second
    GlinkRequest taken;
    TEST_ASSERT_TRUE(glink_mailbox_take(&box, &taken, 110, kPickup));
    TEST_ASSERT_EQUAL(GlinkOp::UPDATE, taken.op);
    TEST_ASSERT_EQUAL_INT32(180, taken.dose_tenths);
    TEST_ASSERT_FALSE(glink_mailbox_submit(&box, update_request(), 111));  // TAKEN
    glink_mailbox_complete(&box, success_result(2));
    TEST_ASSERT_FALSE(glink_mailbox_submit(&box, update_request(), 112));  // DONE, not yet collected
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::DONE, glink_mailbox_poll(&box, 113, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(GlinkReplyKind::SELECT, out.kind);
    TEST_ASSERT_EQUAL_UINT8(2, out.select_profile);
    TEST_ASSERT_FALSE(glink_mailbox_busy(box));
    TEST_ASSERT_TRUE(glink_mailbox_submit(&box, update_request(), 114));
}

static void test_mailbox_take_only_once_and_only_queued() {
    GlinkMailbox box;
    GlinkRequest r;
    TEST_ASSERT_FALSE(glink_mailbox_take(&box, &r, 0, kPickup));  // idle
    glink_mailbox_submit(&box, update_request(), 0);
    TEST_ASSERT_TRUE(glink_mailbox_take(&box, &r, 1, kPickup));
    TEST_ASSERT_FALSE(glink_mailbox_take(&box, &r, 2, kPickup));  // already taken
}

static void test_mailbox_poll_waits_then_withdraws_a_request_nobody_took() {
    GlinkMailbox box;
    glink_mailbox_submit(&box, update_request(), 1000);
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::WAITING, glink_mailbox_poll(&box, 1999, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(GlinkMailboxWait::TIMEOUT, glink_mailbox_poll(&box, 2000, kPickup, kRun, &out));
    TEST_ASSERT_FALSE(glink_mailbox_busy(box));  // withdrawn: free again, nothing happened
    GlinkRequest r;
    TEST_ASSERT_FALSE(glink_mailbox_take(&box, &r, 2001, kPickup));
}

static void test_mailbox_ui_cannot_take_after_the_deadline() {
    GlinkMailbox box;
    glink_mailbox_submit(&box, update_request(), 0);
    GlinkRequest r;
    TEST_ASSERT_FALSE(glink_mailbox_take(&box, &r, kPickup, kPickup));  // late: the poll withdraws it, UI must not apply it
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::TIMEOUT, glink_mailbox_poll(&box, kPickup, kPickup, kRun, &out));
    TEST_ASSERT_FALSE(glink_mailbox_busy(box));
}

static void test_mailbox_taken_request_is_waited_for_then_abandoned() {
    GlinkMailbox box;
    glink_mailbox_submit(&box, update_request(), 0);
    GlinkRequest r;
    TEST_ASSERT_TRUE(glink_mailbox_take(&box, &r, 900, kPickup));
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::WAITING, glink_mailbox_poll(&box, 2899, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(GlinkMailboxWait::TIMEOUT, glink_mailbox_poll(&box, 2900, kPickup, kRun, &out));
    TEST_ASSERT_TRUE(glink_mailbox_busy(box));  // the UI task still owns it
    TEST_ASSERT_FALSE(glink_mailbox_submit(&box, update_request(), 3000));
    glink_mailbox_complete(&box, success_result(1));  // late: dropped, mailbox idle again
    TEST_ASSERT_FALSE(glink_mailbox_busy(box));
    TEST_ASSERT_TRUE(glink_mailbox_submit(&box, update_request(), 3001));
}

static void test_mailbox_idle_poll_reports_timeout_and_stray_complete_is_ignored() {
    GlinkMailbox box;
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::TIMEOUT, glink_mailbox_poll(&box, 0, kPickup, kRun, &out));
    glink_mailbox_complete(&box, success_result(1));
    TEST_ASSERT_FALSE(glink_mailbox_busy(box));
}

static void test_mailbox_is_wrap_safe() {
    GlinkMailbox box;
    const uint32_t start = 0xFFFFFF00u;
    glink_mailbox_submit(&box, update_request(), start);
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::WAITING, glink_mailbox_poll(&box, start + 999u, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(GlinkMailboxWait::TIMEOUT, glink_mailbox_poll(&box, start + 1000u, kPickup, kRun, &out));
}

static void test_mailbox_carries_a_failure_reply() {
    GlinkMailbox box;
    glink_mailbox_submit(&box, update_request(), 0);
    GlinkRequest r;
    glink_mailbox_take(&box, &r, 1, kPickup);
    GlinkMailboxResult fail;
    glink_set_failure(fail.failure, GlinkError::EDIT_IN_PROGRESS, glink_msg::EDIT_IN_PROGRESS, "", 1);
    glink_mailbox_complete(&box, fail);
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::DONE, glink_mailbox_poll(&box, 2, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(GlinkReplyKind::FAILURE, out.kind);
    TEST_ASSERT_EQUAL(GlinkError::EDIT_IN_PROGRESS, out.failure.code);
}

// ---- snapshot freshness: stale means UI_BUSY ----
static void test_snapshot_freshness() {
    TEST_ASSERT_FALSE(glink_snapshot_fresh(false, 0, 10, 1000));  // never published
    TEST_ASSERT_TRUE(glink_snapshot_fresh(true, 100, 1099, 1000));
    TEST_ASSERT_FALSE(glink_snapshot_fresh(true, 100, 1100, 1000));
    TEST_ASSERT_TRUE(glink_snapshot_fresh(true, 0xFFFFFFF0u, 0x10u, 1000));  // wrap
}

// ---- get served from the snapshot ----
static void test_get_all_without_and_with_the_dose_profile() {
    char out[GLINK_MAX_REPLY_BYTES];
    GlinkFailure f;
    GlinkRequest r;
    r.object = GlinkObject::GRIND_PROFILE;
    r.op = GlinkOp::GET;
    size_t n = glink_serve_get(make_snapshot(GlinkScreen::READY, false), r, out, sizeof(out), f);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NULL(strstr(out, "\"index\":3"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"index\":2"));
    n = glink_serve_get(make_snapshot(GlinkScreen::READY, true), r, out, sizeof(out), f);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"index\":3"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"grindUnit\":\"um\""));
}

static void test_get_one_and_missing_dose_profile() {
    char out[GLINK_MAX_REPLY_BYTES];
    GlinkFailure f;
    GlinkRequest r;
    r.object = GlinkObject::GRIND_PROFILE;
    r.op = GlinkOp::GET;
    r.selector_index = true;
    r.selector_value = 1;
    TEST_ASSERT_TRUE(glink_serve_get(make_snapshot(GlinkScreen::READY, false), r, out, sizeof(out), f) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"index\":1"));
    r.selector_value = 3;
    TEST_ASSERT_EQUAL_UINT32(0, glink_serve_get(make_snapshot(GlinkScreen::READY, false), r, out, sizeof(out), f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_FOUND, f.code);
    TEST_ASSERT_EQUAL_STRING("selector.value", f.field);
    TEST_ASSERT_TRUE(glink_serve_get(make_snapshot(GlinkScreen::READY, true), r, out, sizeof(out), f) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"writable\":true"));
}

static void test_get_follows_the_state_table() {
    char out[GLINK_MAX_REPLY_BYTES];
    GlinkRequest r;
    r.object = GlinkObject::GRIND_PROFILE;
    r.op = GlinkOp::GET;
    for (int s = 0; s < static_cast<int>(GlinkScreen::COUNT); s++) {
        const GlinkScreen screen = static_cast<GlinkScreen>(s);
        GlinkFailure f;
        const size_t n = glink_serve_get(make_snapshot(screen, true), r, out, sizeof(out), f);
        if (screen == GlinkScreen::UPDATING) {
            TEST_ASSERT_EQUAL_UINT32(0, n);
            TEST_ASSERT_EQUAL(GlinkError::UPDATING, f.code);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(n > 0, "get is allowed in every state but UPDATING");
        }
    }
}

// ---- screen classification (7.1) ----
static GlinkScreen classify(UIState state, bool overlay = false, bool busy = false, bool update = false, bool lead_in = false) {
    GlinkStateInputs in;
    in.ui_state = state;
    in.overlay_active = overlay;
    in.busy = busy;
    in.update_active = update;
    in.lead_in_active = lead_in;
    return glink_classify_screen(in);
}

static void test_classify_screen() {
    TEST_ASSERT_EQUAL(GlinkScreen::READY, classify(UIState::READY));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::READY, true));          // a dialog or overlay
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::READY, false, true));   // grinder busy
    TEST_ASSERT_EQUAL(GlinkScreen::MENU, classify(UIState::MENU));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::MENU, true));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::MENU, false, true));    // motor test
    TEST_ASSERT_EQUAL(GlinkScreen::EDIT, classify(UIState::EDIT));
    TEST_ASSERT_EQUAL(GlinkScreen::EDIT, classify(UIState::EDIT, true));
    TEST_ASSERT_EQUAL(GlinkScreen::GRINDING, classify(UIState::GRINDING, false, true));
    TEST_ASSERT_EQUAL(GlinkScreen::GRINDING, classify(UIState::GRINDING, true, true));
    TEST_ASSERT_EQUAL(GlinkScreen::GRINDING, classify(UIState::PURGE_CONFIRM));
    TEST_ASSERT_EQUAL(GlinkScreen::DONE, classify(UIState::GRIND_COMPLETE));
    TEST_ASSERT_EQUAL(GlinkScreen::DONE, classify(UIState::GRIND_TIMEOUT));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::CALIBRATION));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::CONFIRM));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::AUTOTUNING));
    TEST_ASSERT_EQUAL(GlinkScreen::OTHER, classify(UIState::OTA_UPDATE_FAILED));
}

static void test_classify_screen_update_and_lead_in_come_first() {
    TEST_ASSERT_EQUAL(GlinkScreen::UPDATING, classify(UIState::READY, false, false, true));
    TEST_ASSERT_EQUAL(GlinkScreen::UPDATING, classify(UIState::GRINDING, false, true, true));
    TEST_ASSERT_EQUAL(GlinkScreen::UPDATING, classify(UIState::OTA_UPDATE));
    TEST_ASSERT_EQUAL(GlinkScreen::LEAD_IN, classify(UIState::READY, true, false, false, true));
    TEST_ASSERT_EQUAL(GlinkScreen::LEAD_IN, classify(UIState::READY, true, false, true, true));  // row order of 7.1
}

static void test_screen_feeds_the_write_table() {
    GlinkFailure f;
    TEST_ASSERT_TRUE(glink_state_check(classify(UIState::READY), GlinkOpClass::UPDATE, 1, f));
    TEST_ASSERT_TRUE(glink_state_check(classify(UIState::MENU), GlinkOpClass::UPDATE, 1, f));
    TEST_ASSERT_FALSE(glink_state_check(classify(UIState::EDIT), GlinkOpClass::UPDATE, 1, f));
    TEST_ASSERT_EQUAL(GlinkError::EDIT_IN_PROGRESS, f.code);
    TEST_ASSERT_FALSE(glink_state_check(classify(UIState::GRINDING, false, true), GlinkOpClass::UPDATE, 1, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);  // update while GRINDING is NOT_READY (docs 7.1)
    TEST_ASSERT_FALSE(glink_state_check(classify(UIState::MENU), GlinkOpClass::SELECT, 1, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
    TEST_ASSERT_FALSE(glink_state_check(classify(UIState::READY, false, false, true), GlinkOpClass::UPDATE, 1, f));
    TEST_ASSERT_EQUAL(GlinkError::UPDATING, f.code);
    TEST_ASSERT_FALSE(glink_state_check(classify(UIState::READY, true, false, false, true), GlinkOpClass::UPDATE, 1, f));
    TEST_ASSERT_EQUAL(GlinkError::COMMAND_IN_FLIGHT, f.code);
}

// ---- phase, start block, periodInfo ----
static void test_phase_mapping() {
    TEST_ASSERT_EQUAL(GlinkPhase::NONE, glink_phase_from_grind(0));   // IDLE
    TEST_ASSERT_EQUAL(GlinkPhase::INITIALIZING, glink_phase_from_grind(1));
    TEST_ASSERT_EQUAL(GlinkPhase::PREDICTIVE, glink_phase_from_grind(5));
    TEST_ASSERT_EQUAL(GlinkPhase::TIME_GRINDING, glink_phase_from_grind(10));
    TEST_ASSERT_EQUAL(GlinkPhase::TIME_ADDITIONAL_PULSE, glink_phase_from_grind(11));
    TEST_ASSERT_EQUAL(GlinkPhase::NONE, glink_phase_from_grind(12));  // COMPLETED
    TEST_ASSERT_EQUAL(GlinkPhase::NONE, glink_phase_from_grind(13));  // TIMEOUT
    TEST_ASSERT_EQUAL(GlinkPhase::PRIME, glink_phase_from_grind(14));
    TEST_ASSERT_EQUAL(GlinkPhase::PRIME_SETTLING, glink_phase_from_grind(15));
    TEST_ASSERT_EQUAL(GlinkPhase::PURGE_CONFIRM, glink_phase_from_grind(16));
    TEST_ASSERT_EQUAL(GlinkPhase::NONE, glink_phase_from_grind(17));
    TEST_ASSERT_EQUAL(GlinkPhase::NONE, glink_phase_from_grind(-1));
}

static void test_start_block_mapping() {
    TEST_ASSERT_EQUAL(GlinkStartBlock::NONE, glink_start_block_from_grind(GrindStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkStartBlock::MOTOR_DISABLED, glink_start_block_from_grind(GrindStartBlock::MOTOR_DISABLED));
    TEST_ASSERT_EQUAL(GlinkStartBlock::UPDATE_LOCK, glink_start_block_from_grind(GrindStartBlock::UPDATE_IN_PROGRESS));
    TEST_ASSERT_EQUAL(GlinkStartBlock::LOAD_CELL_NOT_CONNECTED,
                      glink_start_block_from_grind(GrindStartBlock::LOAD_CELL_NOT_CONNECTED));
    TEST_ASSERT_EQUAL(GlinkStartBlock::LOAD_CELL_NO_DATA, glink_start_block_from_grind(GrindStartBlock::LOAD_CELL_NO_DATA));
    TEST_ASSERT_EQUAL(GlinkStartBlock::LOAD_CELL_RATE_INVALID,
                      glink_start_block_from_grind(GrindStartBlock::LOAD_CELL_RATE_INVALID));
    TEST_ASSERT_EQUAL(GlinkStartBlock::MOTOR_FAULT, glink_start_block_from_grind(GrindStartBlock::MOTOR_FAULT));
}

// The protocol has no motor fault code: a latched deadman fault is refused as NOT_READY, on a start and in periodInfo
static void test_motor_fault_start_block_is_not_ready() {
    const char* message = nullptr;
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, glink_start_block_error(GlinkStartBlock::MOTOR_FAULT, &message));
    TEST_ASSERT_EQUAL_STRING(glink_msg::MOTOR_FAULT, message);
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, glink_blocked(GlinkDevState::WARNING, GlinkStartBlock::MOTOR_FAULT));
    TEST_ASSERT_EQUAL(GlinkDevState::WARNING, glink_dev_state(GlinkScreen::READY, GlinkStartBlock::MOTOR_FAULT));
    GlinkStartInputs in;
    in.profile_exists = true;
    in.stored_dose_tenths = 180;
    in.block = GlinkStartBlock::MOTOR_FAULT;
    GlinkFailure f;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
}

static void test_period_idle_warning_and_grinding() {
    GlinkPeriodInputs in;
    in.screen = GlinkScreen::READY;
    in.weight_valid = true;
    in.weight_g = 7.34f;
    in.select_profile = 2;
    in.result_seq = 4;
    in.profile_rev = 9;
    GlinkPeriodInfo p = glink_build_period(in);
    TEST_ASSERT_EQUAL(GlinkDevState::IDLE, p.dev_state);
    TEST_ASSERT_TRUE(p.weight_valid);
    TEST_ASSERT_EQUAL_INT32(73, p.weight_tenths);
    TEST_ASSERT_EQUAL_UINT8(2, p.select_profile);
    TEST_ASSERT_EQUAL(GlinkError::NONE, p.blocked);
    TEST_ASSERT_FALSE(p.has_target);
    TEST_ASSERT_EQUAL(GlinkPhase::NONE, p.phase);
    TEST_ASSERT_EQUAL_UINT32(4, p.result_seq);
    TEST_ASSERT_EQUAL_UINT32(9, p.profile_rev);

    in.block = GlinkStartBlock::LOAD_CELL_NO_DATA;
    p = glink_build_period(in);
    TEST_ASSERT_EQUAL(GlinkDevState::WARNING, p.dev_state);
    TEST_ASSERT_EQUAL(GlinkError::LOAD_CELL_NO_DATA, p.blocked);

    in.screen = GlinkScreen::GRINDING;
    in.grind_phase_id = 5;
    in.target = 18.04f;
    p = glink_build_period(in);
    TEST_ASSERT_EQUAL(GlinkDevState::GRINDING, p.dev_state);
    TEST_ASSERT_EQUAL(GlinkPhase::PREDICTIVE, p.phase);
    TEST_ASSERT_TRUE(p.has_target);
    TEST_ASSERT_EQUAL_INT32(180, p.target_tenths);
    TEST_ASSERT_EQUAL(GlinkMode::WEIGHT, p.mode);
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, p.blocked);

    in.time_mode = true;
    in.target = 4.5f;
    p = glink_build_period(in);
    TEST_ASSERT_EQUAL(GlinkMode::TIME, p.mode);
    TEST_ASSERT_EQUAL_INT32(45, p.target_tenths);
}

static void test_period_weight_validity_and_rounding() {
    GlinkPeriodInputs in;
    in.screen = GlinkScreen::READY;
    in.weight_valid = false;
    in.weight_g = 5.0f;
    TEST_ASSERT_FALSE(glink_build_period(in).weight_valid);  // invalid reading: null
    in.weight_valid = true;
    in.weight_g = -0.05f;
    TEST_ASSERT_EQUAL_INT32(-1, glink_build_period(in).weight_tenths);  // half away from zero
    in.weight_g = 0.0f / 0.0f;
    TEST_ASSERT_FALSE(glink_build_period(in).weight_valid);  // NaN
    TEST_ASSERT_EQUAL_INT32(180, glink_tenths_from_float(18.0f));
    TEST_ASSERT_EQUAL_INT32(183, glink_tenths_from_float(18.3f));
}

static void test_period_states_without_grinding_have_no_phase_or_target() {
    const GlinkScreen screens[] = {GlinkScreen::MENU, GlinkScreen::EDIT, GlinkScreen::DONE, GlinkScreen::OTHER,
                                   GlinkScreen::UPDATING, GlinkScreen::LEAD_IN};
    for (GlinkScreen screen : screens) {
        GlinkPeriodInputs in;
        in.screen = screen;
        in.grind_phase_id = 5;
        const GlinkPeriodInfo p = glink_build_period(in);
        TEST_ASSERT_EQUAL(GlinkPhase::NONE, p.phase);
        TEST_ASSERT_FALSE(p.has_target);
    }
}

// ---- rate limit with a fake clock ----
static void test_rate_limit_one_stored_change_per_second() {
    GlinkWriteLimiter limiter;
    TEST_ASSERT_TRUE(limiter.ok(0));          // nothing stored since boot, even at clock 0
    limiter.note_store(5000);
    TEST_ASSERT_FALSE(limiter.ok(5000));
    TEST_ASSERT_FALSE(limiter.ok(5999));
    TEST_ASSERT_TRUE(limiter.ok(6000));
    limiter.note_store(6000);
    TEST_ASSERT_FALSE(limiter.ok(6500));
}

static void test_rate_limit_wraps_with_the_clock() {
    GlinkWriteLimiter limiter;
    limiter.note_store(0xFFFFFE00u);
    TEST_ASSERT_FALSE(limiter.ok(0xFFFFFFFFu));
    TEST_ASSERT_FALSE(limiter.ok(0x100u));     // 0x300 = 768 ms later
    TEST_ASSERT_TRUE(limiter.ok(0x200u));      // 1024 ms later
}

// A stored change, then a second one 500 ms later is RATE_LIMITED; an identical write is never limited
static void test_rate_limit_applies_to_stored_changes_only() {
    GlinkWriteLimiter limiter;
    GlinkProfileState current;
    current.exists = true;
    current.dose_tenths = 180;
    GlinkRequest r = update_request();
    r.dose_tenths = 200;
    GlinkUpdatePlan plan;
    GlinkFailure f;
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, current, r, limiter.ok(1000), plan, f));
    TEST_ASSERT_TRUE(plan.changed);
    limiter.note_store(1000);
    current.dose_tenths = 200;
    r.dose_tenths = 210;
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::READY, current, r, limiter.ok(1500), plan, f));
    TEST_ASSERT_EQUAL(GlinkError::RATE_LIMITED, f.code);
    r.dose_tenths = 200;  // identical: success, no write
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, current, r, limiter.ok(1500), plan, f));
    TEST_ASSERT_FALSE(plan.changed);
    r.dose_tenths = 210;
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, current, r, limiter.ok(2000), plan, f));
}

void run_glink_gate_tests() {
    RUN_TEST(test_mailbox_second_submit_is_refused_until_collected);
    RUN_TEST(test_mailbox_take_only_once_and_only_queued);
    RUN_TEST(test_mailbox_poll_waits_then_withdraws_a_request_nobody_took);
    RUN_TEST(test_mailbox_ui_cannot_take_after_the_deadline);
    RUN_TEST(test_mailbox_taken_request_is_waited_for_then_abandoned);
    RUN_TEST(test_mailbox_idle_poll_reports_timeout_and_stray_complete_is_ignored);
    RUN_TEST(test_mailbox_is_wrap_safe);
    RUN_TEST(test_mailbox_carries_a_failure_reply);
    RUN_TEST(test_snapshot_freshness);
    RUN_TEST(test_get_all_without_and_with_the_dose_profile);
    RUN_TEST(test_get_one_and_missing_dose_profile);
    RUN_TEST(test_get_follows_the_state_table);
    RUN_TEST(test_classify_screen);
    RUN_TEST(test_classify_screen_update_and_lead_in_come_first);
    RUN_TEST(test_screen_feeds_the_write_table);
    RUN_TEST(test_phase_mapping);
    RUN_TEST(test_start_block_mapping);
    RUN_TEST(test_motor_fault_start_block_is_not_ready);
    RUN_TEST(test_period_idle_warning_and_grinding);
    RUN_TEST(test_period_weight_validity_and_rounding);
    RUN_TEST(test_period_states_without_grinding_have_no_phase_or_target);
    RUN_TEST(test_rate_limit_one_stored_change_per_second);
    RUN_TEST(test_rate_limit_wraps_with_the_clock);
    RUN_TEST(test_rate_limit_applies_to_stored_changes_only);
}
