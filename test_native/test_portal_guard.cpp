#include <unity.h>
#include <string.h>
#include <string>
#include "network/portal_guard.h"
#include "portal_vectors.h"

namespace {

PortalError error_from_code(const std::string& code) {
    for (size_t i = 0; i < static_cast<size_t>(PortalError::COUNT); i++) {
        if (code == portal_error_info(static_cast<PortalError>(i)).code) return static_cast<PortalError>(i);
    }
    TEST_FAIL_MESSAGE(code.c_str());
    return PortalError::NONE;
}

// null in the vectors = header absent
const char* text_or_null(const PJson& v) { return v.is_null() ? nullptr : v.s.c_str(); }

PortalRoute route_from_name(const std::string& n) {
    if (n == "INFO") return PortalRoute::INFO;
    if (n == "PAGE") return PortalRoute::PAGE;
    if (n == "API_READ") return PortalRoute::API_READ;
    if (n == "API_WRITE") return PortalRoute::API_WRITE;
    if (n == "API_COMMAND") return PortalRoute::API_COMMAND;
    if (n == "UPDATE_PUSH") return PortalRoute::UPDATE_PUSH;
    TEST_FAIL_MESSAGE(n.c_str());
    return PortalRoute::INFO;
}

PortalCommand command_from_name(const std::string& n) {
    if (n == "stop") return PortalCommand::STOP;
    if (n == "tare") return PortalCommand::TARE;
    if (n == "start") return PortalCommand::START;
    if (n == "motor_test") return PortalCommand::MOTOR_TEST;
    TEST_FAIL_MESSAGE(n.c_str());
    return PortalCommand::STOP;
}

PortalUiMode ui_from_name(const std::string& n) {
    if (n == "READY") return PortalUiMode::READY;
    if (n == "MENU") return PortalUiMode::MENU;
    if (n == "OTHER") return PortalUiMode::OTHER;
    TEST_FAIL_MESSAGE(n.c_str());
    return PortalUiMode::OTHER;
}

GrindStartBlock start_block_from_name(const std::string& n) {
    if (n == "NONE") return GrindStartBlock::NONE;
    if (n == "MOTOR_DISABLED") return GrindStartBlock::MOTOR_DISABLED;
    if (n == "UPDATE_IN_PROGRESS") return GrindStartBlock::UPDATE_IN_PROGRESS;
    if (n == "LOAD_CELL_NOT_CONNECTED") return GrindStartBlock::LOAD_CELL_NOT_CONNECTED;
    if (n == "LOAD_CELL_NO_DATA") return GrindStartBlock::LOAD_CELL_NO_DATA;
    if (n == "LOAD_CELL_RATE_INVALID") return GrindStartBlock::LOAD_CELL_RATE_INVALID;
    TEST_FAIL_MESSAGE(n.c_str());
    return GrindStartBlock::NONE;
}

MotorTestBlock motor_block_from_name(const std::string& n) {
    if (n == "NONE") return MotorTestBlock::NONE;
    if (n == "MOTOR_DISABLED") return MotorTestBlock::MOTOR_DISABLED;
    if (n == "UPDATE_IN_PROGRESS") return MotorTestBlock::UPDATE_IN_PROGRESS;
    if (n == "BUSY") return MotorTestBlock::BUSY;
    if (n == "MOTOR_FAULT") return MotorTestBlock::MOTOR_FAULT;
    if (n == "START_FAILED") return MotorTestBlock::START_FAILED;
    TEST_FAIL_MESSAGE(n.c_str());
    return MotorTestBlock::NONE;
}

PortalError expected_error(const PJson& v) { return v.is_null() ? PortalError::NONE : error_from_code(v.s); }

}  // namespace

static void test_host_vectors() {
    const PJson& cases = portal_vectors().at("host_cases");
    TEST_ASSERT_TRUE(cases.a.size() > 60);
    for (const PJson& c : cases.a) {
        const std::string& host = c.at("host").s;
        const PortalError got = portal_check_host(host.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(c.at("ok").b ? static_cast<int>(PortalError::NONE) : static_cast<int>(PortalError::BAD_HOST),
                                  static_cast<int>(got), host.c_str());
    }
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_HOST), static_cast<int>(portal_check_host(nullptr)));
}

static void test_origin_vectors() {
    const PJson& cases = portal_vectors().at("origin_cases");
    for (const PJson& c : cases.a) {
        const char* origin = text_or_null(c.at("origin"));
        const std::string& host = c.at("host").s;
        const PortalError got = portal_check_origin(origin, host.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(c.at("ok").b ? static_cast<int>(PortalError::NONE) : static_cast<int>(PortalError::BAD_ORIGIN),
                                  static_cast<int>(got), origin ? origin : "(absent)");
    }
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_ORIGIN), static_cast<int>(portal_check_origin("http://x.local", nullptr)));
}

static void test_origin_overlong_is_refused() {
    const std::string host(100, 'a');
    const std::string origin = "http://" + host + std::string(200, 'b');
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_ORIGIN), static_cast<int>(portal_check_origin(origin.c_str(), host.c_str())));
}

static void test_content_type_vectors() {
    for (const PJson& c : portal_vectors().at("content_type_cases").a) {
        const char* ct = text_or_null(c.at("content_type"));
        const PortalError got = portal_check_content_type(ct);
        TEST_ASSERT_EQUAL_MESSAGE(c.at("ok").b ? static_cast<int>(PortalError::NONE) : static_cast<int>(PortalError::UNSUPPORTED_MEDIA_TYPE),
                                  static_cast<int>(got), ct ? ct : "(absent)");
    }
    const std::string overlong = "application/json;" + std::string(200, ' ') + "charset=utf-8";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::UNSUPPORTED_MEDIA_TYPE), static_cast<int>(portal_check_content_type(overlong.c_str())));
}

static void test_custom_header_vectors() {
    for (const PJson& c : portal_vectors().at("custom_header_cases").a) {
        const char* v = text_or_null(c.at("value"));
        const PortalError got = portal_check_custom_header(v);
        TEST_ASSERT_EQUAL_MESSAGE(c.at("ok").b ? static_cast<int>(PortalError::NONE) : static_cast<int>(PortalError::MISSING_CUSTOM_HEADER),
                                  static_cast<int>(got), v ? v : "(absent)");
    }
    const PJson& limits = portal_vectors().at("limits");
    TEST_ASSERT_EQUAL_STRING(limits.at("custom_header_name").s.c_str(), PORTAL_CUSTOM_HEADER_NAME);
    TEST_ASSERT_EQUAL_STRING(limits.at("custom_header_value").s.c_str(), PORTAL_CUSTOM_HEADER_VALUE);
}

static void test_check_request_order_and_scope() {
    PortalRequestHeaders h;
    h.host = "grindbyweight.local";
    h.content_type = "application/json";
    h.custom = "1";
    // A read needs only a good Host (and a matching Origin if one is sent)
    PortalRequestHeaders get;
    get.host = "192.168.1.5";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_check_request(get, false)));
    get.origin = "http://evil.com";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_ORIGIN), static_cast<int>(portal_check_request(get, false)));
    // A state-changing call needs all four
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_check_request(h, true)));
    PortalRequestHeaders bad = h;
    bad.custom = nullptr;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::MISSING_CUSTOM_HEADER), static_cast<int>(portal_check_request(bad, true)));
    bad = h;
    bad.content_type = "text/plain";   // the classic cross-site "simple request"
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::UNSUPPORTED_MEDIA_TYPE), static_cast<int>(portal_check_request(bad, true)));
    bad = h;
    bad.origin = "http://evil.com";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_ORIGIN), static_cast<int>(portal_check_request(bad, true)));
    bad = h;
    bad.host = "evil.com";             // DNS rebinding name
    bad.origin = "http://evil.com";    // same-origin from the attacker's point of view
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_HOST), static_cast<int>(portal_check_request(bad, true)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_HOST), static_cast<int>(portal_check_request(bad, false)));
    // Host first, then Origin, then content type, then custom header
    PortalRequestHeaders all_bad;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_HOST), static_cast<int>(portal_check_request(all_bad, true)));
    all_bad.host = "x.local";
    all_bad.origin = "null";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BAD_ORIGIN), static_cast<int>(portal_check_request(all_bad, true)));
    all_bad.origin = nullptr;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::UNSUPPORTED_MEDIA_TYPE), static_cast<int>(portal_check_request(all_bad, true)));
    all_bad.content_type = "application/json";
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::MISSING_CUSTOM_HEADER), static_cast<int>(portal_check_request(all_bad, true)));
}

static void test_gate_vectors() {
    const PJson& cases = portal_vectors().at("gate_cases");
    TEST_ASSERT_EQUAL_UINT32(24, cases.a.size());
    for (const PJson& c : cases.a) {
        const PortalRoute route = route_from_name(c.at("route").s);
        const bool web = c.at("web").b, push = c.at("push").b;
        TEST_ASSERT_EQUAL_MESSAGE(c.at("enabled").b, portal_route_enabled(route, web, push), c.at("route").s.c_str());
        // The refusal of a disabled route (the vector lists none for enabled ones)
        const PortalError want = expected_error(c.at("refusal"));
        const PortalError got = c.at("enabled").b ? PortalError::NONE : portal_route_refusal(route);
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(want), static_cast<int>(got), c.at("route").s.c_str());
    }
}

static void test_gate_keeps_receive_and_web_independent() {
    // Receive off, web on: firmware push stays closed while the panel works
    TEST_ASSERT_FALSE(portal_route_enabled(PortalRoute::UPDATE_PUSH, true, false));
    TEST_ASSERT_TRUE(portal_route_enabled(PortalRoute::API_COMMAND, true, false));
    // Receive on, web off: the panel (including stop) is closed, push is open
    TEST_ASSERT_TRUE(portal_route_enabled(PortalRoute::UPDATE_PUSH, false, true));
    TEST_ASSERT_FALSE(portal_route_enabled(PortalRoute::API_COMMAND, false, true));
    TEST_ASSERT_FALSE(portal_route_enabled(PortalRoute::PAGE, false, true));
    TEST_ASSERT_TRUE(portal_route_enabled(PortalRoute::INFO, false, false));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_route_refusal(PortalRoute::INFO)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_route_refusal(PortalRoute::UPDATE_PUSH)));
}

static void test_server_vectors() {
    const PJson& cases = portal_vectors().at("server_cases");
    TEST_ASSERT_EQUAL_UINT32(8, cases.a.size());
    for (const PJson& c : cases.a) {
        TEST_ASSERT_EQUAL(c.at("run").b, portal_server_should_run(c.at("wifi").b, c.at("web").b, c.at("push").b));
    }
}

static void test_decision_vectors() {
    const PJson& cases = portal_vectors().at("decision_cases");
    TEST_ASSERT_TRUE(cases.a.size() > 30);
    for (const PJson& c : cases.a) {
        const std::string& name = c.at("name").s;
        PortalDecisionInput in;
        in.ui = ui_from_name(c.at("ui").s);
        in.busy = c.at("busy").b;
        in.update_held = c.at("update_held").b;
        in.command_in_flight = c.at("in_flight").b;
        in.start_block = start_block_from_name(c.at("start_block").s);
        in.motor_test_block = motor_block_from_name(c.at("motor_test_block").s);
        const PortalError got = portal_command_decide(command_from_name(c.at("cmd").s), in);
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(expected_error(c.at("error"))), static_cast<int>(got), name.c_str());
    }
}

static void test_write_vectors() {
    for (const PJson& c : portal_vectors().at("write_cases").a) {
        const std::string& name = c.at("name").s;
        PortalDecisionInput in;
        in.ui = ui_from_name(c.at("ui").s);
        in.busy = c.at("busy").b;
        in.command_in_flight = c.at("in_flight").b;
        const PJson* held = c.find("update_held");  // optional, false when absent
        in.update_held = held && held->b;
        // Blocks that matter to commands never matter to settings writes
        in.start_block = GrindStartBlock::MOTOR_DISABLED;
        in.motor_test_block = MotorTestBlock::MOTOR_FAULT;
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(expected_error(c.at("error"))), static_cast<int>(portal_write_decide(in)), name.c_str());
    }
}

static void test_stop_is_accepted_in_every_state() {
    const PortalUiMode modes[] = {PortalUiMode::READY, PortalUiMode::MENU, PortalUiMode::OTHER};
    for (PortalUiMode ui : modes) {
        for (int bits = 0; bits < 8; bits++) {
            PortalDecisionInput in;
            in.ui = ui;
            in.busy = bits & 1;
            in.update_held = bits & 2;
            in.command_in_flight = bits & 4;
            in.start_block = GrindStartBlock::UPDATE_IN_PROGRESS;
            in.motor_test_block = MotorTestBlock::MOTOR_FAULT;
            TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_command_decide(PortalCommand::STOP, in)));
        }
    }
}

// The full tare table (step 6): only an idle READY screen tares, the first applicable refusal wins, and the
// start/motor-test blocks never matter to it
static void test_tare_decision_table() {
    const PortalUiMode modes[] = {PortalUiMode::READY, PortalUiMode::MENU, PortalUiMode::OTHER};
    for (PortalUiMode ui : modes) {
        for (int bits = 0; bits < 8; bits++) {
            PortalDecisionInput in;
            in.ui = ui;
            in.busy = bits & 1;
            in.update_held = bits & 2;
            in.command_in_flight = bits & 4;
            in.start_block = GrindStartBlock::MOTOR_DISABLED;
            in.motor_test_block = MotorTestBlock::MOTOR_FAULT;
            PortalError expected = PortalError::NONE;
            if (in.command_in_flight) expected = PortalError::COMMAND_IN_FLIGHT;
            else if (in.update_held) expected = PortalError::UPDATE_IN_PROGRESS;
            else if (in.busy) expected = PortalError::BUSY;  // refused while grinding (grind, autotune or motor active)
            else if (ui != PortalUiMode::READY) expected = PortalError::NOT_READY;
            TEST_ASSERT_EQUAL(static_cast<int>(expected), static_cast<int>(portal_command_decide(PortalCommand::TARE, in)));
        }
    }
    // the refusals are the documented 409s
    TEST_ASSERT_EQUAL_INT(409, portal_error_info(PortalError::BUSY).status);
    TEST_ASSERT_EQUAL_INT(409, portal_error_info(PortalError::NOT_READY).status);
}

static void test_command_success_status() {
    TEST_ASSERT_EQUAL_INT(200, portal_command_success_status(PortalCommand::STOP));
    TEST_ASSERT_EQUAL_INT(200, portal_command_success_status(PortalCommand::TARE));
    TEST_ASSERT_EQUAL_INT(202, portal_command_success_status(PortalCommand::START));
    TEST_ASSERT_EQUAL_INT(202, portal_command_success_status(PortalCommand::MOTOR_TEST));
}

static void test_lead_in_touch_cancels_only_a_touch_after_the_start() {
    PortalLeadIn l;
    TEST_ASSERT_FALSE(portal_lead_in_touched(l, 5000, 0));  // no lead-in running: nothing to cancel
    portal_lead_in_begin(&l, 10000);
    TEST_ASSERT_FALSE(portal_lead_in_touched(l, 10000, 0));      // age 0 at the very start is not "after"
    TEST_ASSERT_FALSE(portal_lead_in_touched(l, 11000, 1000));   // last touch exactly at the start
    TEST_ASSERT_FALSE(portal_lead_in_touched(l, 11000, 60000));  // an old touch
    TEST_ASSERT_TRUE(portal_lead_in_touched(l, 11000, 999));     // 1 ms after the start
    TEST_ASSERT_TRUE(portal_lead_in_touched(l, 11000, 0));       // touching right now
    // a wrapped millis(): lead-in began just before the wrap, touch just after it
    portal_lead_in_begin(&l, 0xFFFFFF00u);
    TEST_ASSERT_TRUE(portal_lead_in_touched(l, 0x00000100u, 100));
    TEST_ASSERT_FALSE(portal_lead_in_touched(l, 0x00000100u, 600));
}

static void test_lead_in_counts_as_in_flight_for_every_other_command() {
    // The UI task sets command_in_flight while a lead-in runs: all but stop are refused with command_in_flight
    PortalDecisionInput in;
    in.ui = PortalUiMode::READY;
    in.command_in_flight = true;
    const PortalCommand others[] = {PortalCommand::TARE, PortalCommand::START, PortalCommand::MOTOR_TEST};
    for (PortalCommand cmd : others) {
        TEST_ASSERT_EQUAL(static_cast<int>(PortalError::COMMAND_IN_FLIGHT), static_cast<int>(portal_command_decide(cmd, in)));
    }
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::COMMAND_IN_FLIGHT), static_cast<int>(portal_write_decide(in)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_command_decide(PortalCommand::STOP, in)));
    // and even without the flag the overlay classifies the screen as OTHER: not_ready
    in.command_in_flight = false;
    in.ui = PortalUiMode::OTHER;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NOT_READY), static_cast<int>(portal_command_decide(PortalCommand::START, in)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NOT_READY), static_cast<int>(portal_write_decide(in)));
}

static void test_every_start_and_motor_block_maps_to_a_distinct_refusal() {
    const GrindStartBlock starts[] = {GrindStartBlock::MOTOR_DISABLED, GrindStartBlock::UPDATE_IN_PROGRESS,
                                      GrindStartBlock::LOAD_CELL_NOT_CONNECTED, GrindStartBlock::LOAD_CELL_NO_DATA,
                                      GrindStartBlock::LOAD_CELL_RATE_INVALID,
                                      GrindStartBlock::MOTOR_FAULT};
    PortalError seen[8];
    size_t n = 0;
    for (GrindStartBlock b : starts) {
        const PortalError e = portal_error_from_start_block(b);
        TEST_ASSERT_TRUE(e != PortalError::NONE);
        TEST_ASSERT_EQUAL_INT(409, portal_error_info(e).status);
        for (size_t i = 0; i < n; i++) TEST_ASSERT_TRUE(seen[i] != e);
        seen[n++] = e;
    }
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_error_from_start_block(GrindStartBlock::NONE)));
    // A latched deadman fault refuses a web start with the same code as the motor test
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::MOTOR_FAULT),
                      static_cast<int>(portal_error_from_start_block(GrindStartBlock::MOTOR_FAULT)));
    PortalDecisionInput latched;
    latched.ui = PortalUiMode::READY;
    latched.start_block = grind_start_block(true, false, GrindStartFault::NONE, true);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::MOTOR_FAULT),
                      static_cast<int>(portal_command_decide(PortalCommand::START, latched)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BUSY), static_cast<int>(portal_error_from_start_block(static_cast<GrindStartBlock>(99))));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::BUSY), static_cast<int>(portal_error_from_motor_test_block(static_cast<MotorTestBlock>(99))));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalError::NONE), static_cast<int>(portal_error_from_motor_test_block(MotorTestBlock::NONE)));
}

static void test_decision_agrees_with_the_firmware_block_functions() {
    // The pure checks the firmware feeds in: every combination must end in a refusal that names the same cause
    const bool bools[] = {false, true};
    for (bool motor : bools) for (bool locked : bools) {
        for (GrindStartFault fault : {GrindStartFault::NONE, GrindStartFault::NOT_CONNECTED, GrindStartFault::NO_DATA, GrindStartFault::INVALID_SAMPLE_RATE}) {
            PortalDecisionInput in;
            in.ui = PortalUiMode::READY;
            in.start_block = grind_start_block(motor, locked, fault);
            const PortalError e = portal_command_decide(PortalCommand::START, in);
            TEST_ASSERT_EQUAL_MESSAGE(in.start_block == GrindStartBlock::NONE, e == PortalError::NONE, "start");
        }
        for (bool busy : bools) for (bool latched : bools) {
            PortalDecisionInput in;
            in.ui = PortalUiMode::MENU;
            in.motor_test_block = motor_test_block(motor, locked, busy, latched);
            const PortalError e = portal_command_decide(PortalCommand::MOTOR_TEST, in);
            TEST_ASSERT_EQUAL_MESSAGE(in.motor_test_block == MotorTestBlock::NONE, e == PortalError::NONE, "motor test");
        }
    }
}

static void test_lead_in_runs_three_seconds() {
    TEST_ASSERT_EQUAL_UINT32(3000, PORTAL_LEAD_IN_MS);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(portal_vectors().at("limits").at("lead_in_ms").n), PORTAL_LEAD_IN_MS);
    PortalLeadIn l;
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::IDLE), static_cast<int>(portal_lead_in_poll(&l, 100)));
    portal_lead_in_begin(&l, 1000);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 1000)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 3999)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::GO), static_cast<int>(portal_lead_in_poll(&l, 4000)));
    // GO is reported once
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::IDLE), static_cast<int>(portal_lead_in_poll(&l, 4001)));
    TEST_ASSERT_FALSE(l.active);
}

static void test_lead_in_cancel() {
    PortalLeadIn l;
    portal_lead_in_cancel(&l);  // nothing running: ignored, must not poison the next lead-in
    portal_lead_in_begin(&l, 500);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 600)));
    portal_lead_in_cancel(&l);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::CANCELLED), static_cast<int>(portal_lead_in_poll(&l, 700)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::IDLE), static_cast<int>(portal_lead_in_poll(&l, 800)));  // once
    // A cancel that arrives in the same tick as the deadline wins: the touch happened first
    portal_lead_in_begin(&l, 1000);
    portal_lead_in_cancel(&l);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::CANCELLED), static_cast<int>(portal_lead_in_poll(&l, 9000)));
    // A new lead-in after a cancel runs the full time
    portal_lead_in_begin(&l, 2000);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 4999)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::GO), static_cast<int>(portal_lead_in_poll(&l, 5000)));
    // A cancel after GO has nothing to cancel
    portal_lead_in_cancel(&l);
    portal_lead_in_begin(&l, 6000);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 6001)));
}

static void test_lead_in_restart_clears_a_pending_cancel() {
    PortalLeadIn l;
    portal_lead_in_begin(&l, 0);
    portal_lead_in_cancel(&l);
    portal_lead_in_begin(&l, 100);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 200)));
}

static void test_lead_in_millis_wrap() {
    PortalLeadIn l;
    portal_lead_in_begin(&l, 0xFFFFFF00u);
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 0xFFFFFFF0u)));  // before the wrap
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 0x00000100u)));  // 512 ms later
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::WAITING), static_cast<int>(portal_lead_in_poll(&l, 0xFFFFFF00u + 2999u)));
    TEST_ASSERT_EQUAL(static_cast<int>(PortalLeadInState::GO), static_cast<int>(portal_lead_in_poll(&l, 0xFFFFFF00u + 3000u)));
}

static void test_lead_in_remaining_seconds() {
    PortalLeadIn l;
    TEST_ASSERT_EQUAL_UINT32(0, portal_lead_in_remaining_s(l, 0));
    portal_lead_in_begin(&l, 10000);
    TEST_ASSERT_EQUAL_UINT32(3, portal_lead_in_remaining_s(l, 10000));
    TEST_ASSERT_EQUAL_UINT32(3, portal_lead_in_remaining_s(l, 10999));
    TEST_ASSERT_EQUAL_UINT32(2, portal_lead_in_remaining_s(l, 11000));
    TEST_ASSERT_EQUAL_UINT32(1, portal_lead_in_remaining_s(l, 12001));
    TEST_ASSERT_EQUAL_UINT32(1, portal_lead_in_remaining_s(l, 12999));
    TEST_ASSERT_EQUAL_UINT32(0, portal_lead_in_remaining_s(l, 13000));
    TEST_ASSERT_EQUAL_UINT32(0, portal_lead_in_remaining_s(l, 99999));
}

void run_portal_guard_tests() {
    RUN_TEST(test_host_vectors);
    RUN_TEST(test_origin_vectors);
    RUN_TEST(test_origin_overlong_is_refused);
    RUN_TEST(test_content_type_vectors);
    RUN_TEST(test_custom_header_vectors);
    RUN_TEST(test_check_request_order_and_scope);
    RUN_TEST(test_gate_vectors);
    RUN_TEST(test_gate_keeps_receive_and_web_independent);
    RUN_TEST(test_server_vectors);
    RUN_TEST(test_decision_vectors);
    RUN_TEST(test_write_vectors);
    RUN_TEST(test_stop_is_accepted_in_every_state);
    RUN_TEST(test_tare_decision_table);
    RUN_TEST(test_command_success_status);
    RUN_TEST(test_lead_in_touch_cancels_only_a_touch_after_the_start);
    RUN_TEST(test_lead_in_counts_as_in_flight_for_every_other_command);
    RUN_TEST(test_every_start_and_motor_block_maps_to_a_distinct_refusal);
    RUN_TEST(test_decision_agrees_with_the_firmware_block_functions);
    RUN_TEST(test_lead_in_runs_three_seconds);
    RUN_TEST(test_lead_in_cancel);
    RUN_TEST(test_lead_in_restart_clears_a_pending_cancel);
    RUN_TEST(test_lead_in_millis_wrap);
    RUN_TEST(test_lead_in_remaining_seconds);
}
