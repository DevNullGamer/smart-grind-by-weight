#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "bluetooth/glink_decide.h"
#include "bluetooth/glink_mailbox.h"
#include "bluetooth/glink_results.h"
#include "bluetooth/glink_screen.h"
#include "bluetooth/glink_session_map.h"
#include "bluetooth/glink_snapshot.h"
#include "bluetooth/glink_start.h"
#include "network/portal_guard.h"

// Host tests of the GLink start path (chg020 step 6b): the lead-in driven by a fake clock, the exclusion with the web
// panel's lead-in, the repeated decision at GO, the result mapping through the seam, the result feed and the restoration
// of the grind mode after a forced weight-mode session.

namespace {

constexpr uint32_t kT0 = 10000;  // the fake clock when the lead-in begins

struct LeadInRun {
    GlinkLeadInEvent event = GlinkLeadInEvent::NONE;
    uint32_t at_ms = 0;  // milliseconds after the begin of the frame that ended the lead-in (or the last frame)
};

// Runs 1 ms frames from the begin until the lead-in ends. touch_at_ms < 0: never touched (the last touch is long ago).
LeadInRun run_lead_in(int32_t touch_at_ms, uint32_t begin_ms = kT0) {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, begin_ms);
    LeadInRun run;
    for (uint32_t k = 0; k <= 3100; k++) {
        const uint32_t now = begin_ms + k;
        GlinkLeadInInputs in;
        in.touch_available = true;
        in.ms_since_last_touch = (touch_at_ms >= 0 && k >= static_cast<uint32_t>(touch_at_ms))
                                     ? k - static_cast<uint32_t>(touch_at_ms)
                                     : 60000;
        run.event = glink_lead_in_step(&lead_in, in, now);
        run.at_ms = k;
        if (run.event != GlinkLeadInEvent::WAITING) break;
    }
    return run;
}

GlinkLeadInInputs idle_inputs() {
    GlinkLeadInInputs in;
    in.touch_available = true;
    in.ms_since_last_touch = 60000;
    return in;
}

GlinkStartInputs start_inputs(GlinkStartBlock block = GlinkStartBlock::NONE) {
    GlinkStartInputs in;
    in.profile_exists = true;
    in.stored_dose_tenths = 180;
    in.block = block;
    return in;
}

GlinkGrindRecord record(uint8_t outcome, GrindAbortReason reason = GrindAbortReason::NONE, bool stopped = false) {
    GlinkGrindRecord r;
    r.seq = 5;
    r.outcome = outcome;
    r.abort_reason = reason;
    r.stopped = stopped;
    r.final_weight_g = 18.1f;
    r.pulses = 2;
    r.total_time_ms = 14230;
    r.motor_time_ms = 11800;
    r.profile_id = 3;
    r.target_weight_g = 18.0f;
    r.origin = GlinkOrigin::BLE;
    return r;
}

// GlinkOutcome numbering = GrindController::GrindSessionResult (static_asserted in grind_controller.cpp)
constexpr uint8_t kUnknown = 0, kSuccess = 1, kOvershoot = 2, kMaxPulses = 3, kTimeout = 4, kError = 5;

}  // namespace

// ---- lead-in timing with a fake clock ----
static void test_lead_in_without_touch_goes_at_exactly_three_seconds() {
    const LeadInRun run = run_lead_in(-1);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::GO, run.event);
    TEST_ASSERT_EQUAL_UINT32(3000, run.at_ms);  // 2999 is still WAITING
    TEST_ASSERT_EQUAL_UINT32(3000, GLINK_START_LEAD_IN_MS);
}

static void test_lead_in_touch_at_0_ms_is_the_start_itself_and_does_not_cancel() {
    const LeadInRun run = run_lead_in(0);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::GO, run.event);
    TEST_ASSERT_EQUAL_UINT32(3000, run.at_ms);
}

static void test_lead_in_touch_just_after_the_begin_cancels() {
    const LeadInRun run = run_lead_in(1);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_TOUCH, run.event);
    TEST_ASSERT_EQUAL_UINT32(1, run.at_ms);
}

static void test_lead_in_touch_at_2999_ms_cancels() {
    const LeadInRun run = run_lead_in(2999);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_TOUCH, run.event);
    TEST_ASSERT_EQUAL_UINT32(2999, run.at_ms);
}

static void test_lead_in_touch_at_3000_ms_still_wins_over_go() {
    const LeadInRun run = run_lead_in(3000);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_TOUCH, run.event);  // same frame as the time-up: the cancel wins
    TEST_ASSERT_EQUAL_UINT32(3000, run.at_ms);
}

static void test_lead_in_touch_after_go_is_too_late() {
    const LeadInRun run = run_lead_in(3001);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::GO, run.event);
    TEST_ASSERT_EQUAL_UINT32(3000, run.at_ms);
}

static void test_lead_in_old_touch_before_the_begin_does_not_cancel() {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    GlinkLeadInInputs in = idle_inputs();
    in.ms_since_last_touch = 1500;  // touched 1500 ms ago, the lead-in began 1000 ms ago
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::WAITING, glink_lead_in_step(&lead_in, in, kT0 + 1000));
}

static void test_lead_in_runs_across_the_clock_wrap() {
    const uint32_t begin = 0xFFFFFF00u;
    const LeadInRun go = run_lead_in(-1, begin);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::GO, go.event);
    TEST_ASSERT_EQUAL_UINT32(3000, go.at_ms);
    const LeadInRun touched = run_lead_in(2000, begin);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_TOUCH, touched.event);
    TEST_ASSERT_EQUAL_UINT32(2000, touched.at_ms);
}

static void test_lead_in_without_a_touch_driver_is_dropped() {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    GlinkLeadInInputs in = idle_inputs();
    in.touch_available = false;
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_NO_TOUCH, glink_lead_in_step(&lead_in, in, kT0 + 20));
    TEST_ASSERT_FALSE(lead_in.active);
}

// ---- cancel racing GO ----
static void test_stop_at_the_go_frame_cancels_instead_of_starting() {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    GlinkLeadInInputs in = idle_inputs();
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::WAITING, glink_lead_in_step(&lead_in, in, kT0 + 2999));
    in.stop_pending = true;
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_STOP, glink_lead_in_step(&lead_in, in, kT0 + 3000));
    TEST_ASSERT_FALSE(lead_in.active);
    in.stop_pending = false;
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::NONE, glink_lead_in_step(&lead_in, in, kT0 + 3020));  // nothing starts later
}

static void test_overlay_stop_button_at_the_go_frame_cancels() {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    portal_lead_in_cancel(&lead_in);  // the overlay's STOP button, between two frames
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::CANCELLED_USER, glink_lead_in_step(&lead_in, idle_inputs(), kT0 + 3000));
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::NONE, glink_lead_in_step(&lead_in, idle_inputs(), kT0 + 3020));
}

static void test_go_is_reported_once() {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::GO, glink_lead_in_step(&lead_in, idle_inputs(), kT0 + 3000));
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::NONE, glink_lead_in_step(&lead_in, idle_inputs(), kT0 + 3020));
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::NONE, glink_lead_in_step(&lead_in, idle_inputs(), kT0 + 5000));
}

static void test_no_lead_in_no_event() {
    PortalLeadIn lead_in;
    GlinkLeadInInputs in = idle_inputs();
    in.stop_pending = true;
    in.start_allowed = false;
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::NONE, glink_lead_in_step(&lead_in, in, kT0));
}

// ---- switch turned off or a start block appearing during the lead-in ----
static void test_link_switched_off_during_the_lead_in_drops_the_start() {
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    GlinkLeadInInputs in = idle_inputs();
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::WAITING, glink_lead_in_step(&lead_in, in, kT0 + 500));
    in.link_enabled = false;
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::DROPPED_LINK_OFF, glink_lead_in_step(&lead_in, in, kT0 + 520));
    TEST_ASSERT_FALSE(lead_in.active);
    in.link_enabled = true;
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::NONE, glink_lead_in_step(&lead_in, in, kT0 + 3000));  // and it never starts
}

static void test_start_block_appearing_during_the_lead_in_drops_the_start() {
    // The inputs of the loop are the real decision: a block that appears makes glink_decide_start fail
    GlinkFailure f;
    TEST_ASSERT_TRUE(glink_decide_start(GlinkScreen::READY, start_inputs(), f));
    PortalLeadIn lead_in;
    portal_lead_in_begin(&lead_in, kT0);
    GlinkLeadInInputs in = idle_inputs();
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::WAITING, glink_lead_in_step(&lead_in, in, kT0 + 1000));
    in.start_allowed = glink_decide_start(GlinkScreen::READY, start_inputs(GlinkStartBlock::LOAD_CELL_NO_DATA), f);
    TEST_ASSERT_FALSE(in.start_allowed);
    TEST_ASSERT_EQUAL(GlinkLeadInEvent::DROPPED_STATE, glink_lead_in_step(&lead_in, in, kT0 + 1020));
    TEST_ASSERT_FALSE(lead_in.active);
}

static void test_state_change_during_the_lead_in_drops_the_start() {
    const GlinkScreen screens[] = {GlinkScreen::UPDATING, GlinkScreen::MENU, GlinkScreen::EDIT, GlinkScreen::GRINDING,
                                   GlinkScreen::DONE, GlinkScreen::OTHER};
    for (GlinkScreen screen : screens) {
        GlinkFailure f;
        PortalLeadIn lead_in;
        portal_lead_in_begin(&lead_in, kT0);
        GlinkLeadInInputs in = idle_inputs();
        in.start_allowed = glink_decide_start(screen, start_inputs(), f);
        TEST_ASSERT_FALSE(in.start_allowed);
        TEST_ASSERT_EQUAL(GlinkLeadInEvent::DROPPED_STATE, glink_lead_in_step(&lead_in, in, kT0 + 100));
    }
}

static void test_profile_changing_during_the_lead_in_drops_the_start() {
    // expectDose is kept and checked again; a dose that no longer matches (or left 5 to 60 g) fails the decision
    GlinkStartInputs in = start_inputs();
    in.has_expect_dose = true;
    in.expect_dose_tenths = 180;
    GlinkFailure f;
    TEST_ASSERT_TRUE(glink_decide_start(GlinkScreen::READY, in, f));
    in.stored_dose_tenths = 200;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL(GlinkError::CONFLICT, f.code);
    in.stored_dose_tenths = 700;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL(GlinkError::OUT_OF_RANGE, f.code);
}

// ---- the decision repeated at GO: which refusals open the start-block dialog ----
static void test_refusal_at_go_shows_the_dialog_only_for_a_start_block() {
    const GlinkStartBlock blocks[] = {GlinkStartBlock::MOTOR_DISABLED, GlinkStartBlock::UPDATE_LOCK,
                                      GlinkStartBlock::LOAD_CELL_NOT_CONNECTED, GlinkStartBlock::LOAD_CELL_NO_DATA,
                                      GlinkStartBlock::LOAD_CELL_RATE_INVALID};
    for (GlinkStartBlock block : blocks) {
        GlinkFailure f;
        const GlinkStartInputs in = start_inputs(block);
        TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
        TEST_ASSERT_TRUE(glink_refusal_shows_dialog(GlinkScreen::READY, in, f));
    }
}

static void test_refusal_at_go_shows_nothing_for_other_reasons() {
    GlinkFailure f;
    // another screen took over (even with a block present)
    GlinkStartInputs in = start_inputs(GlinkStartBlock::MOTOR_DISABLED);
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::MENU, in, f));
    TEST_ASSERT_FALSE(glink_refusal_shows_dialog(GlinkScreen::MENU, in, f));
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::UPDATING, in, f));
    TEST_ASSERT_FALSE(glink_refusal_shows_dialog(GlinkScreen::UPDATING, in, f));
    // a profile check comes before the block: nothing is shown
    in.profile_exists = false;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL(GlinkError::PROFILE_NOT_SET, f.code);
    TEST_ASSERT_FALSE(glink_refusal_shows_dialog(GlinkScreen::READY, in, f));
    in = start_inputs(GlinkStartBlock::LOAD_CELL_NO_DATA);
    in.stored_dose_tenths = 40;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_FALSE(glink_refusal_shows_dialog(GlinkScreen::READY, in, f));
    in = start_inputs();
    in.has_expect_dose = true;
    in.expect_dose_tenths = 190;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_FALSE(glink_refusal_shows_dialog(GlinkScreen::READY, in, f));
    // accepted: no refusal at all
    TEST_ASSERT_TRUE(glink_decide_start(GlinkScreen::READY, start_inputs(), f));
    TEST_ASSERT_FALSE(glink_refusal_shows_dialog(GlinkScreen::READY, start_inputs(), f));
}

// ---- web panel and BLE lead-in exclude each other ----
static void test_web_and_ble_lead_in_exclude_each_other() {
    // BLE lead-in running: the web panel counts it as a command in flight (PortalUIController::gather_decision)
    PortalDecisionInput web;
    web.ui = PortalUiMode::READY;
    web.command_in_flight = true;
    TEST_ASSERT_EQUAL(PortalError::COMMAND_IN_FLIGHT, portal_command_decide(PortalCommand::START, web));
    TEST_ASSERT_EQUAL(PortalError::COMMAND_IN_FLIGHT, portal_write_decide(web));  // its writes are refused too

    // web lead-in running: the screen is LEAD_IN for GLink (classify) and a BLE start is refused with COMMAND_IN_FLIGHT
    GlinkStateInputs state;
    state.ui_state = UIState::READY;
    state.overlay_active = true;   // the web overlay
    state.lead_in_active = true;
    const GlinkScreen screen = glink_classify_screen(state);
    TEST_ASSERT_EQUAL(GlinkScreen::LEAD_IN, screen);
    GlinkFailure f;
    TEST_ASSERT_FALSE(glink_decide_start(screen, start_inputs(), f));
    TEST_ASSERT_EQUAL(GlinkError::COMMAND_IN_FLIGHT, f.code);
    // ... and a profile write / select is refused the same way
    bool will_store = false;
    TEST_ASSERT_FALSE(glink_decide_select(screen, 1, true, 0, true, &will_store, f));
    TEST_ASSERT_EQUAL(GlinkError::COMMAND_IN_FLIGHT, f.code);
}

static void test_own_lead_in_overlay_is_left_out_when_asking_if_a_start_is_still_allowed() {
    // The lead-in's own overlay and flag make the screen LEAD_IN (periodInfo STARTING); the allowed-check leaves both out
    GlinkStateInputs state;
    state.ui_state = UIState::READY;
    state.overlay_active = true;
    state.lead_in_active = true;
    TEST_ASSERT_EQUAL(GlinkScreen::LEAD_IN, glink_classify_screen(state));
    state.overlay_active = false;
    state.lead_in_active = false;
    TEST_ASSERT_EQUAL(GlinkScreen::READY, glink_classify_screen(state));
}

// ---- STARTING in periodInfo ----
static void test_period_while_starting_carries_lead_in_and_target() {
    GlinkPeriodInputs in;
    in.screen = GlinkScreen::LEAD_IN;
    in.lead_in_known = true;
    in.lead_in_ms = 2400;
    in.target = 18.0f;
    GlinkPeriodInfo p = glink_build_period(in);
    TEST_ASSERT_EQUAL(GlinkDevState::STARTING, p.dev_state);
    TEST_ASSERT_TRUE(p.has_lead_in);
    TEST_ASSERT_EQUAL_UINT32(2400, p.lead_in_ms);
    TEST_ASSERT_TRUE(p.has_target);
    TEST_ASSERT_EQUAL_INT32(180, p.target_tenths);
    TEST_ASSERT_EQUAL(GlinkMode::WEIGHT, p.mode);
    TEST_ASSERT_EQUAL(GlinkError::COMMAND_IN_FLIGHT, p.blocked);
    char out[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_period_info(out, sizeof(out), p);
    TEST_ASSERT_TRUE(n > 0);
    const std::string text(out, n);
    TEST_ASSERT_TRUE(text.find("\"devState\":\"STARTING\"") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("\"leadInMs\":2400") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("\"target\":18.0,\"mode\":\"WEIGHT\"") != std::string::npos);
    in.time_mode = true;  // a web start in TIME mode
    in.target = 25.0f;
    p = glink_build_period(in);
    TEST_ASSERT_EQUAL(GlinkMode::TIME, p.mode);
    TEST_ASSERT_EQUAL_INT32(250, p.target_tenths);
}

static void test_lead_in_remaining_ms_and_text() {
    PortalLeadIn lead_in;
    TEST_ASSERT_EQUAL_UINT32(0, glink_lead_in_remaining_ms(lead_in, kT0));
    portal_lead_in_begin(&lead_in, kT0);
    TEST_ASSERT_EQUAL_UINT32(3000, glink_lead_in_remaining_ms(lead_in, kT0));
    TEST_ASSERT_EQUAL_UINT32(1, glink_lead_in_remaining_ms(lead_in, kT0 + 2999));
    TEST_ASSERT_EQUAL_UINT32(0, glink_lead_in_remaining_ms(lead_in, kT0 + 3000));
    TEST_ASSERT_EQUAL_UINT32(0, glink_lead_in_remaining_ms(lead_in, kT0 + 9000));

    char text[80];
    glink_lead_in_text(text, sizeof(text), "PROFILE", 180, 3);
    TEST_ASSERT_EQUAL_STRING("Remote start: PROFILE 18.0 g in 3 s, touch to cancel", text);
    glink_lead_in_text(text, sizeof(text), "CUSTOM", 55, 1);
    TEST_ASSERT_EQUAL_STRING("Remote start: CUSTOM 5.5 g in 1 s, touch to cancel", text);
    glink_lead_in_text(text, 12, "PROFILE", 180, 3);  // truncated, never overrun
    TEST_ASSERT_EQUAL_UINT32(11, strlen(text));
}

// ---- mailbox: the START answer ----
static void test_mailbox_carries_the_start_answer() {
    GlinkMailbox box;
    GlinkRequest request;
    request.object = GlinkObject::GRIND_CONTROL;
    request.op = GlinkOp::START;
    request.index = 3;
    TEST_ASSERT_TRUE(glink_mailbox_submit(&box, request, 100));
    GlinkRequest taken;
    TEST_ASSERT_TRUE(glink_mailbox_take(&box, &taken, 110, 1000));
    TEST_ASSERT_EQUAL(GlinkOp::START, taken.op);
    GlinkMailboxResult answer;
    answer.kind = GlinkReplyKind::START;
    answer.lead_in_ms = GLINK_START_LEAD_IN_MS;
    glink_mailbox_complete(&box, answer);
    GlinkMailboxResult out;
    TEST_ASSERT_EQUAL(GlinkMailboxWait::DONE, glink_mailbox_poll(&box, 120, 1000, 2000, &out));
    TEST_ASSERT_EQUAL(GlinkReplyKind::START, out.kind);
    char buf[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_start(buf, sizeof(buf), out.lead_in_ms);
    TEST_ASSERT_EQUAL_STRING("{\"response\":{\"grindControl\":{\"result\":\"success\",\"data\":{\"leadInMs\":3000}}}}",
                             std::string(buf, n).c_str());
}

static void test_stop_answer_is_accepted_true() {
    char buf[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_stop(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"response\":{\"grindControl\":{\"result\":\"success\",\"data\":{\"accepted\":true}}}}",
                             std::string(buf, n).c_str());
}

// ---- result codes through the seam ----
static void test_result_matches_the_documented_example() {
    const GlinkResultInfo info = glink_result_from_record(record(kSuccess));
    char buf[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_result_broadcast(buf, sizeof(buf), info);
    TEST_ASSERT_EQUAL_STRING(
        "{\"broadcast\":{\"grindResult\":{\"seq\":5,\"code\":\"OK\",\"origin\":\"BLE\",\"profile\":3,\"mode\":\"WEIGHT\","
        "\"target\":18.0,\"finalWeight\":18.1,\"weightSettled\":true,\"pulses\":2,\"durationMs\":14230,\"motorMs\":11800}}}",
        std::string(buf, n).c_str());
}

static void test_result_codes_for_every_outcome() {
    struct Case {
        uint8_t outcome;
        GrindAbortReason reason;
        bool stopped;
        GlinkResultCode code;
        bool settled;
    };
    const Case cases[] = {
        {kSuccess, GrindAbortReason::NONE, false, GlinkResultCode::OK, true},
        {kOvershoot, GrindAbortReason::NONE, false, GlinkResultCode::OVERSHOOT, true},
        {kMaxPulses, GrindAbortReason::NONE, false, GlinkResultCode::MAX_PULSES, true},
        {kTimeout, GrindAbortReason::NONE, false, GlinkResultCode::TIMEOUT, false},  // the 60 s timeout
        {kUnknown, GrindAbortReason::NONE, true, GlinkResultCode::STOPPED, false},   // a stop is outcome UNKNOWN + stopped
        {kTimeout, GrindAbortReason::NONE, true, GlinkResultCode::STOPPED, false},   // a stop that fell into the timeout
        {kSuccess, GrindAbortReason::NONE, true, GlinkResultCode::OK, true},         // a real end beats a racing stop
        {kError, GrindAbortReason::STALL, false, GlinkResultCode::ERR_STALLED, false},
        {kError, GrindAbortReason::PRIME_FLOW, false, GlinkResultCode::ERR_NO_FLOW, false},
        {kError, GrindAbortReason::PREDICTIVE_FLOW, false, GlinkResultCode::ERR_NO_FLOW, false},
        {kError, GrindAbortReason::SENSOR, false, GlinkResultCode::ERR_SENSOR, false},
        {kError, GrindAbortReason::NONE, false, GlinkResultCode::ERR_SENSOR, false},
        {kError, GrindAbortReason::TARE, false, GlinkResultCode::ERR_TARE, false},
        {kError, GrindAbortReason::MOTOR_FAULT, false, GlinkResultCode::ERR_MOTOR, false},
        {kError, GrindAbortReason::NEG_WEIGHT, false, GlinkResultCode::ERR_NEG_WEIGHT, false},
        {kError, GrindAbortReason::UPDATE, false, GlinkResultCode::UPDATE, false},
    };
    for (const Case& c : cases) {
        const GlinkResultInfo info = glink_result_from_record(record(c.outcome, c.reason, c.stopped));
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.code), static_cast<int>(info.code), glink_result_code_name(c.code));
        TEST_ASSERT_EQUAL_MESSAGE(c.settled, info.weight_settled, glink_result_code_name(c.code));
    }
}

static void test_result_fields_origin_mode_and_rounding() {
    GlinkGrindRecord r = record(kSuccess);
    r.origin = GlinkOrigin::WEB;
    r.profile_id = 2;
    r.time_mode = true;
    r.target_time_ms = 7500;
    r.final_weight_g = -0.3f;
    r.pulses = 255;
    GlinkResultInfo info = glink_result_from_record(r);
    TEST_ASSERT_EQUAL(GlinkOrigin::WEB, info.origin);
    TEST_ASSERT_EQUAL_UINT8(2, info.profile);
    TEST_ASSERT_EQUAL(GlinkMode::TIME, info.mode);
    TEST_ASSERT_EQUAL_INT32(75, info.target_tenths);  // seconds in TIME mode
    TEST_ASSERT_EQUAL_INT32(-3, info.final_weight_tenths);
    TEST_ASSERT_EQUAL_UINT32(255, info.pulses);
    TEST_ASSERT_EQUAL_UINT32(5, info.seq);
    r.final_weight_g = 1.0f / 0.0f;  // never a non-finite number on the wire
    info = glink_result_from_record(r);
    TEST_ASSERT_EQUAL_INT32(0, info.final_weight_tenths);
}

static void test_origin_of_a_session() {
    TEST_ASSERT_EQUAL(GlinkOrigin::BLE, glink_origin_of_session(7, 7, 3));
    TEST_ASSERT_EQUAL(GlinkOrigin::WEB, glink_origin_of_session(3, 7, 3));
    TEST_ASSERT_EQUAL(GlinkOrigin::TOUCH, glink_origin_of_session(8, 7, 3));  // the touch start after both
    TEST_ASSERT_EQUAL(GlinkOrigin::TOUCH, glink_origin_of_session(0, 0, 0));  // no recorded start must not match session 0
    TEST_ASSERT_EQUAL(GlinkOrigin::TOUCH, glink_origin_of_session(1, 0, 0));
}

// ---- result get and broadcast feed ----
static void test_result_get_from_the_snapshot() {
    GlinkSnapshot snap;
    snap.screen = GlinkScreen::READY;
    char buf[GLINK_MAX_REPLY_BYTES + 1];
    GlinkFailure f;
    TEST_ASSERT_EQUAL_UINT32(0, glink_serve_result_get(snap, buf, sizeof(buf), f));  // none since boot
    TEST_ASSERT_EQUAL(GlinkError::NOT_FOUND, f.code);
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkObject::GRIND_RESULT), f.object);

    snap.has_result = true;
    snap.last_result = glink_result_from_record(record(kOvershoot));
    const size_t n = glink_serve_result_get(snap, buf, sizeof(buf), f);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(std::string(buf, n).find("\"code\":\"OVERSHOOT\"") != std::string::npos);
    TEST_ASSERT_TRUE(std::string(buf, n).find("{\"response\":{\"grindResult\":") == 0);

    snap.screen = GlinkScreen::LEAD_IN;  // get is accepted in every state but UPDATING
    TEST_ASSERT_TRUE(glink_serve_result_get(snap, buf, sizeof(buf), f) > 0);
    snap.screen = GlinkScreen::GRINDING;
    TEST_ASSERT_TRUE(glink_serve_result_get(snap, buf, sizeof(buf), f) > 0);
    snap.screen = GlinkScreen::UPDATING;
    TEST_ASSERT_EQUAL_UINT32(0, glink_serve_result_get(snap, buf, sizeof(buf), f));
    TEST_ASSERT_EQUAL(GlinkError::UPDATING, f.code);
}

static void test_result_feed_broadcasts_each_finished_grind_once() {
    GlinkResultFeed feed;
    feed.reset();
    feed.on_handshake();
    TEST_ASSERT_FALSE(feed.due(false, 0));  // baseline: nothing yet
    TEST_ASSERT_FALSE(feed.due(false, 0));
    TEST_ASSERT_TRUE(feed.due(true, 1));    // the first grind finishes
    feed.mark_sent(1);
    TEST_ASSERT_FALSE(feed.due(true, 1));   // once
    TEST_ASSERT_FALSE(feed.due(true, 1));
    TEST_ASSERT_TRUE(feed.due(true, 2));
    TEST_ASSERT_TRUE(feed.due(true, 2));    // a failed attempt may simply ask again
    feed.mark_sent(2);
    TEST_ASSERT_FALSE(feed.due(true, 2));
}

static void test_result_feed_does_not_replay_an_old_result_at_the_handshake() {
    GlinkResultFeed feed;
    feed.reset();
    feed.on_handshake();
    TEST_ASSERT_FALSE(feed.due(true, 4));  // seq 4 finished before this client was handshaken: resultSeq and get cover it
    TEST_ASSERT_FALSE(feed.due(true, 4));
    TEST_ASSERT_TRUE(feed.due(true, 5));
    // a repeated appHello records the baseline again
    feed.mark_sent(5);
    feed.on_handshake();
    TEST_ASSERT_FALSE(feed.due(true, 5));
    TEST_ASSERT_TRUE(feed.due(true, 6));
}

static void test_result_feed_forgets_everything_on_a_new_connection() {
    GlinkResultFeed feed;
    feed.on_handshake();
    feed.due(true, 3);
    feed.mark_sent(3);
    feed.reset();  // connect or disconnect
    feed.on_handshake();
    TEST_ASSERT_FALSE(feed.due(true, 3));  // the new client gets no replay of seq 3
    TEST_ASSERT_TRUE(feed.due(true, 4));
}

static void test_result_feed_first_grind_after_boot_with_no_baseline_result() {
    GlinkResultFeed feed;
    feed.on_handshake();
    TEST_ASSERT_FALSE(feed.due(false, 0));
    TEST_ASSERT_TRUE(feed.due(true, 1));  // seq 1 is not mistaken for the baseline
}

// ---- grind mode restored after a forced weight-mode session ----
static void test_forced_mode_restores_a_time_mode_user() {
    GlinkForcedMode guard;
    GrindMode restore = GrindMode::WEIGHT;
    TEST_ASSERT_FALSE(guard.on_ready(GrindMode::TIME, &restore));  // a touch or web grind: nothing to restore
    TEST_ASSERT_EQUAL(GrindMode::WEIGHT, restore);                 // untouched

    guard.arm();  // a GLink start in WEIGHT mode for a user whose stored mode is TIME
    TEST_ASSERT_TRUE(guard.armed());
    TEST_ASSERT_TRUE(guard.on_ready(GrindMode::TIME, &restore));
    TEST_ASSERT_EQUAL(GrindMode::TIME, restore);
    TEST_ASSERT_FALSE(guard.armed());
    TEST_ASSERT_FALSE(guard.on_ready(GrindMode::TIME, &restore));  // once per session
}

static void test_forced_mode_restores_the_mode_stored_at_the_end_not_at_the_start() {
    GlinkForcedMode guard;
    guard.arm();
    GrindMode restore = GrindMode::WEIGHT;
    TEST_ASSERT_TRUE(guard.on_ready(GrindMode::WEIGHT, &restore));  // a weight-mode user: restored to the same value
    TEST_ASSERT_EQUAL(GrindMode::WEIGHT, restore);
}

static void test_idle_stop_is_forgotten_unless_a_start_is_queued() {
    TEST_ASSERT_TRUE(glink_consume_idle_stop(false, false));
    TEST_ASSERT_FALSE(glink_consume_idle_stop(false, true));  // the queued start's lead-in must see this stop
    TEST_ASSERT_FALSE(glink_consume_idle_stop(true, false));
    TEST_ASSERT_FALSE(glink_consume_idle_stop(true, true));
}

void run_glink_start_tests() {
    RUN_TEST(test_idle_stop_is_forgotten_unless_a_start_is_queued);
    RUN_TEST(test_lead_in_without_touch_goes_at_exactly_three_seconds);
    RUN_TEST(test_lead_in_touch_at_0_ms_is_the_start_itself_and_does_not_cancel);
    RUN_TEST(test_lead_in_touch_just_after_the_begin_cancels);
    RUN_TEST(test_lead_in_touch_at_2999_ms_cancels);
    RUN_TEST(test_lead_in_touch_at_3000_ms_still_wins_over_go);
    RUN_TEST(test_lead_in_touch_after_go_is_too_late);
    RUN_TEST(test_lead_in_old_touch_before_the_begin_does_not_cancel);
    RUN_TEST(test_lead_in_runs_across_the_clock_wrap);
    RUN_TEST(test_lead_in_without_a_touch_driver_is_dropped);
    RUN_TEST(test_stop_at_the_go_frame_cancels_instead_of_starting);
    RUN_TEST(test_overlay_stop_button_at_the_go_frame_cancels);
    RUN_TEST(test_go_is_reported_once);
    RUN_TEST(test_no_lead_in_no_event);
    RUN_TEST(test_link_switched_off_during_the_lead_in_drops_the_start);
    RUN_TEST(test_start_block_appearing_during_the_lead_in_drops_the_start);
    RUN_TEST(test_state_change_during_the_lead_in_drops_the_start);
    RUN_TEST(test_profile_changing_during_the_lead_in_drops_the_start);
    RUN_TEST(test_refusal_at_go_shows_the_dialog_only_for_a_start_block);
    RUN_TEST(test_refusal_at_go_shows_nothing_for_other_reasons);
    RUN_TEST(test_web_and_ble_lead_in_exclude_each_other);
    RUN_TEST(test_own_lead_in_overlay_is_left_out_when_asking_if_a_start_is_still_allowed);
    RUN_TEST(test_period_while_starting_carries_lead_in_and_target);
    RUN_TEST(test_lead_in_remaining_ms_and_text);
    RUN_TEST(test_mailbox_carries_the_start_answer);
    RUN_TEST(test_stop_answer_is_accepted_true);
    RUN_TEST(test_result_matches_the_documented_example);
    RUN_TEST(test_result_codes_for_every_outcome);
    RUN_TEST(test_result_fields_origin_mode_and_rounding);
    RUN_TEST(test_origin_of_a_session);
    RUN_TEST(test_result_get_from_the_snapshot);
    RUN_TEST(test_result_feed_broadcasts_each_finished_grind_once);
    RUN_TEST(test_result_feed_does_not_replay_an_old_result_at_the_handshake);
    RUN_TEST(test_result_feed_forgets_everything_on_a_new_connection);
    RUN_TEST(test_result_feed_first_grind_after_boot_with_no_baseline_result);
    RUN_TEST(test_forced_mode_restores_a_time_mode_user);
    RUN_TEST(test_forced_mode_restores_the_mode_stored_at_the_end_not_at_the_start);
}
