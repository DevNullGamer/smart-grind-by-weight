#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "bluetooth/glink_decide.h"
#include "bluetooth/glink_results.h"
#include "glink_vectors.h"

// Host tests of the GLink decisions (src/bluetooth/glink_decide.*) and the result code mapping
// (src/bluetooth/glink_results.*): the state table of 7.1, the start order of 7.2, the write rules of 5.4 and 7.3,
// devState / blocked (6.1, 6.5).

static GlinkScreen screen_from_name(const std::string& n) {
    static const char* const names[] = {"LEAD_IN", "UPDATING", "READY", "MENU", "EDIT", "GRINDING", "DONE", "OTHER"};
    for (int i = 0; i < 8; i++) {
        if (n == names[i]) return static_cast<GlinkScreen>(i);
    }
    TEST_FAIL_MESSAGE(n.c_str());
    return GlinkScreen::OTHER;
}

static GlinkError error_from_name(const std::string& n) {
    for (int i = 1; i < static_cast<int>(GlinkError::COUNT); i++) {
        if (n == glink_error_code(static_cast<GlinkError>(i))) return static_cast<GlinkError>(i);
    }
    TEST_FAIL_MESSAGE(n.c_str());
    return GlinkError::NONE;
}

// ---- 7.1: every cell of the table ----
static void test_state_table_matches_vectors() {
    const GJson& rows = glink_vectors().at("write_rules");
    TEST_ASSERT_EQUAL_UINT32(static_cast<size_t>(GlinkScreen::COUNT), rows.a.size());
    const char* const cols[] = {"get", "update", "select", "start"};
    for (const GJson& row : rows.a) {
        const GlinkScreen screen = screen_from_name(row.at("state").s);
        for (int c = 0; c < 4; c++) {
            const std::string want = row.at(cols[c]).s;
            GlinkFailure f;
            const bool ok = glink_state_check(screen, static_cast<GlinkOpClass>(c), 1, f);
            const std::string label = row.at("state").s + "/" + cols[c];
            if (want == "accept" || want == "decide") {
                TEST_ASSERT_TRUE_MESSAGE(ok, label.c_str());
            } else {
                TEST_ASSERT_FALSE_MESSAGE(ok, label.c_str());
                TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), glink_error_code(f.code), label.c_str());
                TEST_ASSERT_EQUAL_INT(1, f.object);
                TEST_ASSERT_TRUE(f.message[0] != '\0');
            }
        }
    }
}

static void test_state_check_out_of_range_screen_refuses() {
    GlinkFailure f;
    TEST_ASSERT_FALSE(glink_state_check(GlinkScreen::COUNT, GlinkOpClass::GET, 1, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
}

// ---- gate ----
static GlinkRequest request_of(GlinkObject o, GlinkOp op) {
    GlinkRequest r;
    r.object = o;
    r.op = op;
    return r;
}

static void test_gate() {
    GlinkFailure f;
    // appHello passes before the handshake; stop passes always, even while a request is pending.
    TEST_ASSERT_TRUE(glink_gate(false, false, request_of(GlinkObject::APP_HELLO, GlinkOp::HANDSHAKE), f));
    TEST_ASSERT_TRUE(glink_gate(false, true, request_of(GlinkObject::GRIND_CONTROL, GlinkOp::STOP), f));
    TEST_ASSERT_TRUE(glink_gate(true, true, request_of(GlinkObject::GRIND_CONTROL, GlinkOp::STOP), f));
    // Everything else needs the handshake first (INVALID_STATE wins over BUSY) ...
    for (GlinkObject o : {GlinkObject::GRIND_PROFILE, GlinkObject::GRIND_CONTROL, GlinkObject::GRIND_RESULT}) {
        GlinkFailure g;
        TEST_ASSERT_FALSE(glink_gate(false, true, request_of(o, GlinkOp::GET), g));
        TEST_ASSERT_EQUAL(GlinkError::INVALID_STATE, g.code);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(o), g.object);
    }
    // ... and a second request while one is pending is BUSY.
    TEST_ASSERT_FALSE(glink_gate(true, true, request_of(GlinkObject::GRIND_PROFILE, GlinkOp::GET), f));
    TEST_ASSERT_EQUAL(GlinkError::BUSY, f.code);
    TEST_ASSERT_FALSE(glink_gate(true, true, request_of(GlinkObject::GRIND_CONTROL, GlinkOp::START), f));
    TEST_ASSERT_EQUAL(GlinkError::BUSY, f.code);
    TEST_ASSERT_TRUE(glink_gate(true, false, request_of(GlinkObject::GRIND_PROFILE, GlinkOp::GET), f));
    // A repeated appHello while one is pending is BUSY too.
    TEST_ASSERT_FALSE(glink_gate(true, true, request_of(GlinkObject::APP_HELLO, GlinkOp::HANDSHAKE), f));
    TEST_ASSERT_EQUAL(GlinkError::BUSY, f.code);
}

// ---- 7.3 ----
static void test_write_interval() {
    TEST_ASSERT_TRUE(glink_write_interval_ok(false, 0, 0));
    TEST_ASSERT_FALSE(glink_write_interval_ok(true, 5000, 5000));
    TEST_ASSERT_FALSE(glink_write_interval_ok(true, 5000, 5999));
    TEST_ASSERT_TRUE(glink_write_interval_ok(true, 5000, 6000));
    TEST_ASSERT_TRUE(glink_write_interval_ok(true, 5000, 60000));
    // Clock wrap: 0xFFFFFF00 to 0x00000100 is 512 ms, to 0x00000300 is 1024 ms.
    TEST_ASSERT_FALSE(glink_write_interval_ok(true, 0xFFFFFF00u, 0x100u));
    TEST_ASSERT_TRUE(glink_write_interval_ok(true, 0xFFFFFF00u, 0x300u));
}

// ---- 7.2 start order ----
static GlinkStartInputs good_start() {
    GlinkStartInputs in;
    in.profile_exists = true;
    in.stored_dose_tenths = 180;
    return in;
}

static void test_start_accepts_when_everything_is_fine() {
    GlinkFailure f;
    TEST_ASSERT_TRUE(glink_decide_start(GlinkScreen::READY, good_start(), f));
    GlinkStartInputs in = good_start();
    in.has_expect_dose = true;
    in.expect_dose_tenths = 180;
    TEST_ASSERT_TRUE(glink_decide_start(GlinkScreen::READY, in, f));
    for (int32_t dose : {50, 600}) {  // both ends of the BLE range
        in.stored_dose_tenths = in.expect_dose_tenths = dose;
        TEST_ASSERT_TRUE(glink_decide_start(GlinkScreen::READY, in, f));
    }
}

static void test_start_order_matches_vectors() {
    // Each refusal of the list is produced with every later (lower priority) condition also present, and every earlier
    // one absent, then the earlier ones are added one at a time: the first listed always wins.
    const GJson& order = glink_vectors().at("start_order");
    TEST_ASSERT_EQUAL_UINT32(11, order.a.size());
    struct Step {
        GlinkScreen screen;
        bool exists;
        int32_t stored;
        bool expect_wrong;
        GlinkStartBlock block;
    };
    // Conditions that trigger refusal number i (0 based) of 7.2.
    const Step triggers[] = {
        {GlinkScreen::LEAD_IN, true, 180, false, GlinkStartBlock::NONE},
        {GlinkScreen::UPDATING, true, 180, false, GlinkStartBlock::NONE},
        {GlinkScreen::MENU, true, 180, false, GlinkStartBlock::NONE},
        {GlinkScreen::READY, false, 180, false, GlinkStartBlock::NONE},
        {GlinkScreen::READY, true, 700, false, GlinkStartBlock::NONE},
        {GlinkScreen::READY, true, 180, true, GlinkStartBlock::NONE},
        {GlinkScreen::READY, true, 180, false, GlinkStartBlock::MOTOR_DISABLED},
        {GlinkScreen::READY, true, 180, false, GlinkStartBlock::UPDATE_LOCK},
        {GlinkScreen::READY, true, 180, false, GlinkStartBlock::LOAD_CELL_NOT_CONNECTED},
        {GlinkScreen::READY, true, 180, false, GlinkStartBlock::LOAD_CELL_NO_DATA},
        {GlinkScreen::READY, true, 180, false, GlinkStartBlock::LOAD_CELL_RATE_INVALID},
    };
    for (size_t i = 0; i < order.a.size(); i++) {
        GlinkFailure f;
        GlinkStartInputs in;
        in.profile_exists = triggers[i].exists;
        in.stored_dose_tenths = triggers[i].stored;
        in.has_expect_dose = triggers[i].expect_wrong;
        in.expect_dose_tenths = 190;
        in.block = triggers[i].block;
        TEST_ASSERT_FALSE(glink_decide_start(triggers[i].screen, in, f));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(order.a[i].s.c_str(), glink_error_code(f.code), order.a[i].s.c_str());
    }
    // Priority: with all later conditions present, refusal i is still the one reported.
    for (size_t i = 0; i < order.a.size(); i++) {
        GlinkFailure f;
        GlinkStartInputs in;
        GlinkScreen screen = GlinkScreen::READY;
        in.profile_exists = true;
        in.stored_dose_tenths = 180;
        in.block = GlinkStartBlock::NONE;
        for (size_t j = i; j < order.a.size(); j++) {  // all conditions from i on
            if (j == 3) in.profile_exists = false;
            if (j == 4) in.stored_dose_tenths = 700;
            if (j == 5) {
                in.has_expect_dose = true;
                in.expect_dose_tenths = 190;
            }
            if (j >= 6 && in.block == GlinkStartBlock::NONE) in.block = triggers[j].block;
        }
        if (i <= 2) screen = triggers[i].screen;  // the screen conditions are exclusive: the first one wins
        TEST_ASSERT_FALSE(glink_decide_start(screen, in, f));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(order.a[i].s.c_str(), glink_error_code(f.code), order.a[i].s.c_str());
    }
}

static void test_start_failure_details() {
    GlinkFailure f;
    GlinkStartInputs in = good_start();
    in.profile_exists = false;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL_STRING("profile", f.field);
    in = good_start();
    in.stored_dose_tenths = 49;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL_STRING("dose", f.field);
    in.stored_dose_tenths = 601;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL(GlinkError::OUT_OF_RANGE, f.code);
    in = good_start();
    in.has_expect_dose = true;
    in.expect_dose_tenths = 181;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL(GlinkError::CONFLICT, f.code);
    TEST_ASSERT_EQUAL_STRING("expectDose", f.field);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GlinkObject::GRIND_CONTROL), f.object);
    in = good_start();
    in.block = GlinkStartBlock::LOAD_CELL_NO_DATA;
    TEST_ASSERT_FALSE(glink_decide_start(GlinkScreen::READY, in, f));
    TEST_ASSERT_EQUAL_STRING("", f.field);
}

// ---- update ----
static GlinkProfileState state_with(int32_t dose, bool size, int32_t size_t10, const char* unit) {
    GlinkProfileState s;
    s.exists = true;
    s.dose_tenths = dose;
    s.has_grind_size = size;
    s.grind_size_tenths = size_t10;
    snprintf(s.grind_unit, sizeof(s.grind_unit), "%s", unit);
    return s;
}

static GlinkRequest update_of(bool dose, int32_t d, bool size, bool size_null, int32_t s, bool unit, const char* u) {
    GlinkRequest r = request_of(GlinkObject::GRIND_PROFILE, GlinkOp::UPDATE);
    r.index = 3;
    r.has_dose = dose;
    r.dose_tenths = d;
    r.has_grind_size = size;
    r.grind_size_null = size_null;
    r.grind_size_tenths = s;
    r.has_grind_unit = unit;
    snprintf(r.grind_unit, sizeof(r.grind_unit), "%s", u);
    return r;
}

static void test_update_plan() {
    GlinkUpdatePlan plan;
    GlinkFailure f;
    const GlinkProfileState cur = state_with(180, true, 145, "um");
    // Dose only: size and unit stay.
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, cur, update_of(true, 185, false, false, 0, false, ""), true, plan, f));
    TEST_ASSERT_TRUE(plan.changed);
    TEST_ASSERT_EQUAL_INT32(185, plan.result.dose_tenths);
    TEST_ASSERT_TRUE(plan.result.has_grind_size);
    TEST_ASSERT_EQUAL_STRING("um", plan.result.grind_unit);
    TEST_ASSERT_EQUAL_UINT8(3, plan.result.index);
    // Identical values: success, nothing changed, and never rate limited.
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, cur, update_of(true, 180, true, false, 145, true, "um"), false, plan, f));
    TEST_ASSERT_FALSE(plan.changed);
    // grindSize null clears size and unit together.
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, cur, update_of(false, 0, true, true, 0, false, ""), true, plan, f));
    TEST_ASSERT_TRUE(plan.changed);
    TEST_ASSERT_FALSE(plan.result.has_grind_size);
    TEST_ASSERT_EQUAL_STRING("", plan.result.grind_unit);
    // Unit only cleared: size stays.
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, cur, update_of(false, 0, false, false, 0, true, ""), true, plan, f));
    TEST_ASSERT_TRUE(plan.changed);
    TEST_ASSERT_TRUE(plan.result.has_grind_size);
    TEST_ASSERT_EQUAL_STRING("", plan.result.grind_unit);
    // A new size on a profile without unit stays without unit; a unit is then added.
    const GlinkProfileState bare = state_with(180, false, 0, "");
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, bare, update_of(false, 0, true, false, 100, true, "clicks"), true, plan, f));
    TEST_ASSERT_EQUAL_STRING("clicks", plan.result.grind_unit);
    TEST_ASSERT_EQUAL_INT32(100, plan.result.grind_size_tenths);
    // Size 0.0 is a size, not "none".
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, bare, update_of(false, 0, true, false, 0, false, ""), true, plan, f));
    TEST_ASSERT_TRUE(plan.changed);
    TEST_ASSERT_TRUE(plan.result.has_grind_size);
}

static void test_update_refusals() {
    GlinkUpdatePlan plan;
    GlinkFailure f;
    const GlinkProfileState cur = state_with(180, true, 145, "um");
    const GlinkProfileState none;  // profile 3 does not exist
    // Create needs a dose.
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::READY, none, update_of(false, 0, true, false, 145, false, ""), true, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::MISSING_FIELD, f.code);
    TEST_ASSERT_EQUAL_STRING("dose", f.field);
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, none, update_of(true, 180, false, false, 0, false, ""), true, plan, f));
    TEST_ASSERT_TRUE(plan.changed);  // creating always changes
    // A unit needs a size.
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::READY, state_with(180, false, 0, ""), update_of(false, 0, false, false, 0, true, "um"), true, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_EQUAL_STRING("grindUnit", f.field);
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::READY, cur, update_of(false, 0, true, true, 0, true, "um"), true, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::READY, none, update_of(true, 180, false, false, 0, true, "um"), true, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    // State rules come first.
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::EDIT, none, update_of(false, 0, false, false, 0, true, "um"), true, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::EDIT_IN_PROGRESS, f.code);
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::GRINDING, cur, update_of(true, 190, false, false, 0, false, ""), true, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::MENU, cur, update_of(true, 190, false, false, 0, false, ""), true, plan, f));
    // Rate limit applies to a change only.
    TEST_ASSERT_FALSE(glink_decide_update(GlinkScreen::READY, cur, update_of(true, 190, false, false, 0, false, ""), false, plan, f));
    TEST_ASSERT_EQUAL(GlinkError::RATE_LIMITED, f.code);
    TEST_ASSERT_TRUE(glink_decide_update(GlinkScreen::READY, cur, update_of(true, 180, false, false, 0, false, ""), false, plan, f));
}

// ---- select ----
static void test_select() {
    GlinkFailure f;
    bool will_store = true;
    TEST_ASSERT_TRUE(glink_decide_select(GlinkScreen::READY, 2, false, 1, true, &will_store, f));
    TEST_ASSERT_TRUE(will_store);
    TEST_ASSERT_TRUE(glink_decide_select(GlinkScreen::READY, 1, false, 1, false, &will_store, f));  // same: no write, no limit
    TEST_ASSERT_FALSE(will_store);
    TEST_ASSERT_FALSE(glink_decide_select(GlinkScreen::READY, 2, false, 1, false, &will_store, f));
    TEST_ASSERT_EQUAL(GlinkError::RATE_LIMITED, f.code);
    TEST_ASSERT_FALSE(will_store);
    TEST_ASSERT_FALSE(glink_decide_select(GlinkScreen::READY, 3, false, 1, true, &will_store, f));
    TEST_ASSERT_EQUAL(GlinkError::PROFILE_NOT_SET, f.code);
    TEST_ASSERT_EQUAL_STRING("index", f.field);
    TEST_ASSERT_TRUE(glink_decide_select(GlinkScreen::READY, 3, true, 0, true, &will_store, f));
    TEST_ASSERT_FALSE(glink_decide_select(GlinkScreen::MENU, 2, false, 1, true, &will_store, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
    TEST_ASSERT_FALSE(glink_decide_select(GlinkScreen::EDIT, 2, false, 1, true, &will_store, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
    // The state rule wins over PROFILE_NOT_SET.
    TEST_ASSERT_FALSE(glink_decide_select(GlinkScreen::GRINDING, 3, false, 1, true, &will_store, f));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, f.code);
}

// ---- devState / blocked ----
static void test_dev_state_and_blocked() {
    TEST_ASSERT_EQUAL(GlinkDevState::STARTING, glink_dev_state(GlinkScreen::LEAD_IN, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::UPDATING, glink_dev_state(GlinkScreen::UPDATING, GlinkStartBlock::MOTOR_DISABLED));
    TEST_ASSERT_EQUAL(GlinkDevState::IDLE, glink_dev_state(GlinkScreen::READY, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::WARNING, glink_dev_state(GlinkScreen::READY, GlinkStartBlock::LOAD_CELL_NO_DATA));
    TEST_ASSERT_EQUAL(GlinkDevState::SETTING, glink_dev_state(GlinkScreen::MENU, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::SETTING, glink_dev_state(GlinkScreen::EDIT, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::SETTING, glink_dev_state(GlinkScreen::OTHER, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::GRINDING, glink_dev_state(GlinkScreen::GRINDING, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::DONE, glink_dev_state(GlinkScreen::DONE, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkDevState::UNKNOWN, glink_dev_state(GlinkScreen::COUNT, GlinkStartBlock::NONE));

    // 6.5: blocked is the code a start would return right now. Check against the real start decision for every screen.
    for (int s = 0; s < static_cast<int>(GlinkScreen::COUNT); s++) {
        for (int b = 0; b <= static_cast<int>(GlinkStartBlock::MOTOR_FAULT); b++) {
            const GlinkScreen screen = static_cast<GlinkScreen>(s);
            const GlinkStartBlock block = static_cast<GlinkStartBlock>(b);
            GlinkFailure f;
            const bool accepted = glink_decide_start(screen, good_start(), f);
            const GlinkError blocked = glink_blocked(glink_dev_state(screen, block), block);
            GlinkStartInputs in = good_start();
            in.block = block;
            const bool accepted_with_block = glink_decide_start(screen, in, f);
            if (accepted_with_block) {
                TEST_ASSERT_EQUAL(GlinkError::NONE, blocked);
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(f.code, blocked, "blocked equals the start refusal");
            }
            (void)accepted;
        }
    }
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, glink_blocked(GlinkDevState::UNKNOWN, GlinkStartBlock::NONE));
    TEST_ASSERT_EQUAL(GlinkError::NOT_READY, glink_blocked(GlinkDevState::DONE, GlinkStartBlock::MOTOR_DISABLED));
}

static void test_start_block_error() {
    const char* msg = nullptr;
    TEST_ASSERT_EQUAL(GlinkError::NONE, glink_start_block_error(GlinkStartBlock::NONE, &msg));
    TEST_ASSERT_EQUAL_STRING("", msg);
    TEST_ASSERT_EQUAL(GlinkError::MOTOR_DISABLED, glink_start_block_error(GlinkStartBlock::MOTOR_DISABLED, &msg));
    TEST_ASSERT_EQUAL(GlinkError::UPDATING, glink_start_block_error(GlinkStartBlock::UPDATE_LOCK, &msg));
    TEST_ASSERT_EQUAL(GlinkError::UPDATING, glink_start_block_error(GlinkStartBlock::UPDATE_LOCK, nullptr));
    // Every blocked value of the vectors is the code of a start block.
    for (const GJson& b : glink_vectors().at("blocked").a) {
        bool found = b.s == "NOT_READY" || b.s == "COMMAND_IN_FLIGHT";
        for (int i = 1; i <= static_cast<int>(GlinkStartBlock::MOTOR_FAULT); i++) {
            found = found || b.s == glink_error_code(glink_start_block_error(static_cast<GlinkStartBlock>(i), &msg));
        }
        TEST_ASSERT_TRUE_MESSAGE(found, b.s.c_str());
    }
}

// ---- result mapping: every outcome times every abort reason times stopped ----
static void test_result_mapping_is_complete() {
    const GlinkOutcome outcomes[] = {GlinkOutcome::UNKNOWN, GlinkOutcome::SUCCESS, GlinkOutcome::OVERSHOOT,
                                     GlinkOutcome::MAX_PULSES, GlinkOutcome::TIMEOUT, GlinkOutcome::ERROR};
    const GlinkAbortReason reasons[] = {GlinkAbortReason::NONE,           GlinkAbortReason::SENSOR,
                                        GlinkAbortReason::TARE,           GlinkAbortReason::PRIME_FLOW,
                                        GlinkAbortReason::PREDICTIVE_FLOW, GlinkAbortReason::STALL,
                                        GlinkAbortReason::MOTOR_FAULT,    GlinkAbortReason::NEG_WEIGHT,
                                        GlinkAbortReason::UPDATE};
    for (GlinkOutcome o : outcomes) {
        for (GlinkAbortReason r : reasons) {
            for (bool stopped : {false, true}) {
                GlinkResultCode want;
                if (stopped && (o == GlinkOutcome::UNKNOWN || o == GlinkOutcome::TIMEOUT)) {
                    want = GlinkResultCode::STOPPED;
                } else if (o == GlinkOutcome::SUCCESS) {
                    want = GlinkResultCode::OK;
                } else if (o == GlinkOutcome::OVERSHOOT) {
                    want = GlinkResultCode::OVERSHOOT;
                } else if (o == GlinkOutcome::MAX_PULSES) {
                    want = GlinkResultCode::MAX_PULSES;
                } else if (o != GlinkOutcome::ERROR) {
                    want = GlinkResultCode::TIMEOUT;  // UNKNOWN / TIMEOUT without a stop
                } else {
                    switch (r) {
                        case GlinkAbortReason::STALL: want = GlinkResultCode::ERR_STALLED; break;
                        case GlinkAbortReason::PRIME_FLOW:
                        case GlinkAbortReason::PREDICTIVE_FLOW: want = GlinkResultCode::ERR_NO_FLOW; break;
                        case GlinkAbortReason::TARE: want = GlinkResultCode::ERR_TARE; break;
                        case GlinkAbortReason::MOTOR_FAULT: want = GlinkResultCode::ERR_MOTOR; break;
                        case GlinkAbortReason::NEG_WEIGHT: want = GlinkResultCode::ERR_NEG_WEIGHT; break;
                        case GlinkAbortReason::UPDATE: want = GlinkResultCode::UPDATE; break;
                        default: want = GlinkResultCode::ERR_SENSOR; break;
                    }
                }
                TEST_ASSERT_EQUAL(want, glink_result_code(o, r, stopped));
            }
        }
    }
}

static void test_result_mapping_pinned() {
    TEST_ASSERT_EQUAL(GlinkResultCode::STOPPED, glink_result_code(GlinkOutcome::UNKNOWN, GlinkAbortReason::NONE, true));
    TEST_ASSERT_EQUAL(GlinkResultCode::TIMEOUT, glink_result_code(GlinkOutcome::UNKNOWN, GlinkAbortReason::NONE, false));
    TEST_ASSERT_EQUAL(GlinkResultCode::OK, glink_result_code(GlinkOutcome::SUCCESS, GlinkAbortReason::NONE, true));  // a real outcome wins over a late stop
    TEST_ASSERT_EQUAL(GlinkResultCode::ERR_MOTOR, glink_result_code(GlinkOutcome::ERROR, GlinkAbortReason::MOTOR_FAULT, true));
    TEST_ASSERT_EQUAL(GlinkResultCode::ERR_SENSOR, glink_result_code(GlinkOutcome::ERROR, GlinkAbortReason::NONE, false));
    TEST_ASSERT_EQUAL(GlinkResultCode::ERR_NO_FLOW, glink_result_code(GlinkOutcome::ERROR, GlinkAbortReason::PRIME_FLOW, false));
    TEST_ASSERT_EQUAL(GlinkResultCode::ERR_NO_FLOW, glink_result_code(GlinkOutcome::ERROR, GlinkAbortReason::PREDICTIVE_FLOW, false));
    // weightSettled (5.6): true for OK, OVERSHOOT, MAX_PULSES only.
    for (int i = 0; i < static_cast<int>(GlinkResultCode::COUNT); i++) {
        const GlinkResultCode c = static_cast<GlinkResultCode>(i);
        const bool want = c == GlinkResultCode::OK || c == GlinkResultCode::OVERSHOOT || c == GlinkResultCode::MAX_PULSES;
        TEST_ASSERT_EQUAL(want, glink_result_weight_settled(c));
    }
}

void run_glink_decide_tests() {
    RUN_TEST(test_state_table_matches_vectors);
    RUN_TEST(test_state_check_out_of_range_screen_refuses);
    RUN_TEST(test_gate);
    RUN_TEST(test_write_interval);
    RUN_TEST(test_start_accepts_when_everything_is_fine);
    RUN_TEST(test_start_order_matches_vectors);
    RUN_TEST(test_start_failure_details);
    RUN_TEST(test_update_plan);
    RUN_TEST(test_update_refusals);
    RUN_TEST(test_select);
    RUN_TEST(test_dev_state_and_blocked);
    RUN_TEST(test_start_block_error);
    RUN_TEST(test_result_mapping_is_complete);
    RUN_TEST(test_result_mapping_pinned);
}
