#include <unity.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "bluetooth/glink_decide.h"
#include "bluetooth/glink_messages.h"
#include "glink_vectors.h"

// Host tests of the GLink request parser and the encoders (src/bluetooth/glink_messages.*, glink_decide.*) against
// tools/tests/glink_vectors.json: the dose / grind size / unit cases, one request/response pair per object, op and
// error code, and the broadcast examples.

static std::string tenths_text(int32_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%s%d.%d", v < 0 ? "-" : "", (v < 0 ? -v : v) / 10, (v < 0 ? -v : v) % 10);
    return b;
}

static bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

//------------------------------------------------------------------------------
// Vocabularies and limits
//------------------------------------------------------------------------------
static void test_vocabularies_match_vectors() {
    const GJson& v = glink_vectors();
    const GJson& errors = v.at("errorCode");
    TEST_ASSERT_EQUAL_UINT32(errors.a.size(), static_cast<size_t>(GlinkError::COUNT) - 1);
    for (size_t i = 0; i < errors.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(errors.a[i].s.c_str(), glink_error_code(static_cast<GlinkError>(i + 1)));
    }
    const GJson& states = v.at("devState");
    TEST_ASSERT_EQUAL_UINT32(states.a.size(), static_cast<size_t>(GlinkDevState::COUNT));
    for (size_t i = 0; i < states.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(states.a[i].s.c_str(), glink_dev_state_name(static_cast<GlinkDevState>(i)));
    }
    const GJson& phases = v.at("phase");
    TEST_ASSERT_EQUAL_UINT32(phases.a.size(), static_cast<size_t>(GlinkPhase::COUNT) - 1);
    for (size_t i = 0; i < phases.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(phases.a[i].s.c_str(), glink_phase_name(static_cast<GlinkPhase>(i + 1)));
    }
    const GJson& codes = v.at("resultCode");
    TEST_ASSERT_EQUAL_UINT32(codes.a.size(), static_cast<size_t>(GlinkResultCode::COUNT));
    for (size_t i = 0; i < codes.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(codes.a[i].s.c_str(), glink_result_code_name(static_cast<GlinkResultCode>(i)));
    }
    const GJson& origins = v.at("origin");
    for (size_t i = 0; i < origins.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(origins.a[i].s.c_str(), glink_origin_name(static_cast<GlinkOrigin>(i)));
    }
    const GJson& modes = v.at("mode");
    for (size_t i = 0; i < modes.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(modes.a[i].s.c_str(), glink_mode_name(static_cast<GlinkMode>(i)));
    }
    const GJson& names = v.at("profile_names");
    for (size_t i = 0; i < names.a.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(names.a[i].s.c_str(), glink_profile_name(static_cast<uint8_t>(i)));
    }
    TEST_ASSERT_EQUAL_STRING("", glink_profile_name(4));
    for (const auto& kv : v.at("objects").o) {
        bool found = false;
        for (int i = 0; i < static_cast<int>(GlinkObject::COUNT); i++) {
            found = found || kv.first == glink_object_name(static_cast<GlinkObject>(i));
        }
        TEST_ASSERT_TRUE_MESSAGE(found, kv.first.c_str());
    }
}

static void test_derived_limits_match_constants() {
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(GLINK_DOSE_MIN_G * 10 + 0.5f), GLINK_DOSE_MIN_TENTHS);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(GLINK_DOSE_MAX_G * 10 + 0.5f), GLINK_DOSE_MAX_TENTHS);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(GLINK_GRIND_SIZE_MAX * 10 + 0.5f), GLINK_GRIND_SIZE_MAX_TENTHS);
    const GJson& limits = glink_vectors().at("limits");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(limits.at("GLINK_MAX_REQUEST_BYTES").n), GLINK_MAX_REQUEST_BYTES);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(limits.at("GLINK_GRIND_UNIT_MAX_LEN").n), GLINK_GRIND_UNIT_MAX_LEN);
}

//------------------------------------------------------------------------------
// Dose, grind size and unit cases
//------------------------------------------------------------------------------
static std::string update_request(const char* field, const std::string& json) {
    return std::string("{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"") + field + "\":" + json + "}}}";
}

static void expect_failure(const GJson& expect, const GlinkFailure& f, const char* what) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expect.at("error").s.c_str(), glink_error_code(f.code), what);
    const GJson* field = expect.find("field");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(field ? field->s.c_str() : "", f.field, what);
}

static void run_value_cases(const char* list, const char* field) {
    for (const GJson& c : glink_vectors().at(list).a) {
        const std::string text = update_request(field, c.at("json").s);
        GlinkRequest req;
        GlinkFailure f;
        const bool ok = glink_parse_request(text.data(), text.size(), req, f);
        const GJson& expect = c.at("expect");
        const char* name = c.at("name").s.c_str();
        TEST_ASSERT_EQUAL_MESSAGE(expect.at("ok").b, ok, name);
        if (!ok) {
            expect_failure(expect, f, name);
            continue;
        }
        const GJson& value = expect.at("value");
        if (strcmp(field, "dose") == 0) {
            TEST_ASSERT_TRUE_MESSAGE(req.has_dose, name);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(value.s.c_str(), tenths_text(req.dose_tenths).c_str(), name);
        } else if (strcmp(field, "grindSize") == 0) {
            TEST_ASSERT_TRUE_MESSAGE(req.has_grind_size, name);
            if (value.is_null()) {
                TEST_ASSERT_TRUE_MESSAGE(req.grind_size_null, name);
            } else {
                TEST_ASSERT_FALSE_MESSAGE(req.grind_size_null, name);
                TEST_ASSERT_EQUAL_STRING_MESSAGE(value.s.c_str(), tenths_text(req.grind_size_tenths).c_str(), name);
            }
        } else {
            TEST_ASSERT_TRUE_MESSAGE(req.has_grind_unit, name);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(value.is_null() ? "" : value.s.c_str(), req.grind_unit, name);
        }
    }
}

static void test_dose_cases() { run_value_cases("dose_cases", "dose"); }
static void test_grind_size_cases() { run_value_cases("grind_size_cases", "grindSize"); }
static void test_grind_unit_cases() { run_value_cases("grind_unit_cases", "grindUnit"); }

static void test_expect_dose_uses_the_dose_rules() {
    for (const GJson& c : glink_vectors().at("dose_cases").a) {
        const std::string json = c.at("json").s;
        const std::string text = "{\"request\":{\"grindControl\":{\"op\":\"start\",\"profile\":1,\"expectDose\":" + json + "}}}";
        GlinkRequest req;
        GlinkFailure f;
        const bool ok = glink_parse_request(text.data(), text.size(), req, f);
        const GJson& expect = c.at("expect");
        TEST_ASSERT_EQUAL_MESSAGE(expect.at("ok").b, ok, c.at("name").s.c_str());
        if (ok) {
            TEST_ASSERT_EQUAL_STRING(expect.at("value").s.c_str(), tenths_text(req.expect_dose_tenths).c_str());
        } else {
            TEST_ASSERT_EQUAL_STRING(expect.at("error").s.c_str(), glink_error_code(f.code));
            const GJson* field = expect.find("field");
            TEST_ASSERT_EQUAL_STRING(field ? "expectDose" : "", f.field);
        }
    }
}

//------------------------------------------------------------------------------
// Parser rules not covered by the pairs
//------------------------------------------------------------------------------
static bool parse(const std::string& text, GlinkRequest& req, GlinkFailure& f) {
    return glink_parse_request(text.data(), text.size(), req, f);
}

static void test_oversize_request_is_payload_too_large() {
    std::string text = "{\"request\":{\"appHello\":{\"op\":\"handshake\"}}}";
    text.resize(GLINK_MAX_REQUEST_BYTES, ' ');
    GlinkRequest req;
    GlinkFailure f;
    TEST_ASSERT_TRUE(parse(text, req, f));  // exactly 512 bytes with trailing blanks is fine
    text.push_back(' ');
    TEST_ASSERT_FALSE(parse(text, req, f));
    TEST_ASSERT_EQUAL(GlinkError::PAYLOAD_TOO_LARGE, f.code);
}

static void test_parsed_fields() {
    GlinkRequest req;
    GlinkFailure f;
    TEST_ASSERT_TRUE(parse("{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3.0,\"dose\":18.05,"
                           "\"grindSize\":14.5,\"grindUnit\":\"  um \"}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkObject::GRIND_PROFILE, req.object);
    TEST_ASSERT_EQUAL(GlinkOp::UPDATE, req.op);
    TEST_ASSERT_EQUAL_UINT8(3, req.index);
    TEST_ASSERT_EQUAL_INT32(181, req.dose_tenths);
    TEST_ASSERT_EQUAL_INT32(145, req.grind_size_tenths);
    TEST_ASSERT_EQUAL_STRING("um", req.grind_unit);

    TEST_ASSERT_TRUE(parse("{\"request\":{\"grindProfile\":{\"op\":\"get\",\"selector\":{\"type\":\"index\",\"value\":2}}}}", req, f));
    TEST_ASSERT_TRUE(req.selector_index);
    TEST_ASSERT_EQUAL_UINT8(2, req.selector_value);
    TEST_ASSERT_TRUE(parse("{\"request\":{\"grindProfile\":{\"op\":\"get\"}}}", req, f));
    TEST_ASSERT_FALSE(req.selector_index);

    TEST_ASSERT_TRUE(parse("{\"request\":{\"grindControl\":{\"op\":\"start\",\"profile\":2,\"expectDose\":21.5}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkOp::START, req.op);
    TEST_ASSERT_EQUAL_UINT8(2, req.index);
    TEST_ASSERT_TRUE(req.has_expect_dose);
    TEST_ASSERT_EQUAL_INT32(215, req.expect_dose_tenths);
}

static void test_failures_carry_the_object() {
    GlinkRequest req;
    GlinkFailure f;
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"nope\"}}}", req, f));
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkObject::GRIND_PROFILE), f.object);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"zzz\":{}}}", req, f));
    TEST_ASSERT_EQUAL(-1, f.object);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":5}}", req, f));
    TEST_ASSERT_EQUAL(static_cast<int>(GlinkObject::GRIND_PROFILE), f.object);
    TEST_ASSERT_EQUAL_STRING("grindProfile", f.field);
}

static void test_op_variants() {
    GlinkRequest req;
    GlinkFailure f;
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":5}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":null}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"GET\"}}}", req, f));  // case-sensitive
    TEST_ASSERT_EQUAL(GlinkError::UNKNOWN_OP, f.code);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"start\"}}}", req, f));  // another object's op
    TEST_ASSERT_EQUAL(GlinkError::UNKNOWN_OP, f.code);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindResult\":{\"op\":\"stop\"}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::UNKNOWN_OP, f.code);
    // appHello and grindResult ignore unknown keys; grindControl stop ignores everything else.
    TEST_ASSERT_TRUE(parse("{\"request\":{\"appHello\":{\"op\":\"handshake\",\"client\":\"x\"}}}", req, f));
    TEST_ASSERT_TRUE(parse("{\"request\":{\"grindResult\":{\"op\":\"get\",\"x\":1}}}", req, f));
    TEST_ASSERT_TRUE(parse("{\"request\":{\"grindControl\":{\"op\":\"stop\",\"profile\":99,\"dose\":\"x\"}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkOp::STOP, req.op);
    // A stop still needs the envelope.
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindControl\":{\"op\":\"stop\"},\"grindResult\":{}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindControl\":{\"op\":\"stop\"}},\"x\":1}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    // Unknown keys are refused before required fields are looked at.
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"select\",\"colour\":1}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_EQUAL_STRING("colour", f.field);
    // Keys that are not allowed for the op count as unknown.
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"select\",\"index\":1,\"dose\":18.0}}}", req, f));
    TEST_ASSERT_EQUAL_STRING("dose", f.field);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"get\",\"index\":1}}}", req, f));
    TEST_ASSERT_EQUAL_STRING("index", f.field);
}

static void test_envelope_order() {
    GlinkRequest req;
    GlinkFailure f;
    // The first top-level member that is not "request" is the one named.
    TEST_ASSERT_FALSE(parse("{\"b\":1,\"a\":2,\"request\":{}}", req, f));
    TEST_ASSERT_EQUAL_STRING("b", f.field);
    TEST_ASSERT_FALSE(parse("{\"request\":{},\"a\":2,\"b\":1}", req, f));
    TEST_ASSERT_EQUAL_STRING("a", f.field);
    // Two objects: the count is the error, not the names.
    TEST_ASSERT_FALSE(parse("{\"request\":{\"zzz\":1,\"yyy\":2}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::INVALID_PARAM, f.code);
    TEST_ASSERT_EQUAL_STRING("request", f.field);
    // Whole-request rejection: a dose error stops the parse before the second field is looked at.
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"dose\":61,\"grindSize\":-5}}}", req, f));
    TEST_ASSERT_EQUAL_STRING("dose", f.field);
    // Order of fields for update: index, read-only, dose, grindSize, grindUnit.
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":1,\"dose\":61}}}", req, f));
    TEST_ASSERT_EQUAL(GlinkError::PROFILE_READ_ONLY, f.code);
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"update\",\"index\":3,\"grindSize\":-5,\"grindUnit\":\"a;\"}}}", req, f));
    TEST_ASSERT_EQUAL_STRING("grindSize", f.field);
}

static void test_field_path_is_bounded() {
    // A 32 character key behind "selector." fits the field buffer.
    const std::string key(32, 'k');
    GlinkRequest req;
    GlinkFailure f;
    TEST_ASSERT_FALSE(parse("{\"request\":{\"grindProfile\":{\"op\":\"get\",\"selector\":{\"" + key + "\":1}}}}", req, f));
    TEST_ASSERT_EQUAL_STRING(("selector." + key).c_str(), f.field);
    char out[256];
    TEST_ASSERT_TRUE(glink_encode_failure(out, sizeof(out), f) > 0);
}

//------------------------------------------------------------------------------
// Pairs: a small stand-in for the UI task answers each parsed request from the state described in the vector
//------------------------------------------------------------------------------
struct Model {
    GlinkScreen screen = GlinkScreen::READY;
    bool handshaken = true;
    bool pending = false;
    bool ui_busy = false;
    bool store_fails = false;
    GlinkProfileState p3;
    int32_t dose[3] = {90, 180, 215};
    uint8_t selected = 1;
    GlinkStartBlock block = GlinkStartBlock::NONE;
    bool have_last = false;
    uint32_t last_store = 0;
    bool has_result = false;
};

static const uint32_t NOW = 100000;

static GlinkProfileState state_with_um() {
    GlinkProfileState p;
    p.exists = true;
    p.dose_tenths = 180;
    p.has_grind_size = true;
    p.grind_size_tenths = 145;
    snprintf(p.grind_unit, sizeof(p.grind_unit), "%s", "um");
    return p;
}

static Model model_from_state(const std::string& s) {
    Model m;
    m.handshaken = contains(s, "handshaken");
    if (contains(s, "MENU")) m.screen = GlinkScreen::MENU;
    if (contains(s, "GRINDING")) m.screen = GlinkScreen::GRINDING;
    if (contains(s, "grind-complete")) m.screen = GlinkScreen::DONE;
    if (contains(s, "EDIT screen")) m.screen = GlinkScreen::EDIT;
    if (contains(s, "lead-in running")) m.screen = GlinkScreen::LEAD_IN;
    if (contains(s, "update in progress")) m.screen = GlinkScreen::UPDATING;
    if (contains(s, "calibration screen")) m.screen = GlinkScreen::OTHER;
    if (contains(s, "motor disabled")) m.block = GlinkStartBlock::MOTOR_DISABLED;
    if (contains(s, "load cell not connected")) m.block = GlinkStartBlock::LOAD_CELL_NOT_CONNECTED;
    if (contains(s, "load cell sends no data")) m.block = GlinkStartBlock::LOAD_CELL_NO_DATA;
    if (contains(s, "load cell sample rate invalid")) m.block = GlinkStartBlock::LOAD_CELL_RATE_INVALID;
    if (contains(s, "a stored change 300 ms ago")) {
        m.have_last = true;
        m.last_store = NOW - 300;
    }
    m.store_fails = contains(s, "flash write fails");
    m.has_result = contains(s, "a grind finished");
    m.pending = contains(s, "still being processed");
    m.ui_busy = contains(s, "does not answer");
    const size_t sel = s.find("selected ");
    if (sel != std::string::npos) m.selected = static_cast<uint8_t>(s[sel + 9] - '0');
    const size_t p2 = s.find("profile 2 = ");
    if (p2 != std::string::npos) m.dose[2] = static_cast<int32_t>(atof(s.c_str() + p2 + 12) * 10 + 0.5);
    const size_t p3 = s.find("profile 3 = ");
    if (p3 != std::string::npos || contains(s, "profile 3 set")) {
        m.p3.exists = true;
        m.p3.dose_tenths = 180;
        if (p3 != std::string::npos) {
            const char* q = s.c_str() + p3 + 12;
            m.p3.dose_tenths = static_cast<int32_t>(atof(q) * 10 + 0.5);
            if (contains(s, "no grind size")) {
                m.p3.has_grind_size = false;
            } else {
                const char* sp = strstr(q, " g ");
                if (sp) {
                    m.p3.has_grind_size = true;
                    m.p3.grind_size_tenths = static_cast<int32_t>(atof(sp + 3) * 10 + 0.5);
                    const char* unit = strchr(sp + 3, ' ');
                    if (unit) {
                        std::string u(unit + 1);
                        const size_t comma = u.find(',');
                        if (comma != std::string::npos) u.resize(comma);
                        snprintf(m.p3.grind_unit, sizeof(m.p3.grind_unit), "%s", u.c_str());
                    }
                }
            }
        }
    }
    // "handshaken, MENU" does not say, but its update pair changes a profile 3 that has a size and a unit.
    if (m.screen == GlinkScreen::MENU && !contains(s, "profile 3")) m.p3 = state_with_um();
    return m;
}

static GlinkProfileEntry entry_of(const Model& m, uint8_t index) {
    GlinkProfileEntry e;
    e.index = index;
    if (index < 3) {
        e.dose_tenths = m.dose[index];
    } else {
        e.dose_tenths = m.p3.dose_tenths;
        e.has_grind_size = m.p3.has_grind_size;
        e.grind_size_tenths = m.p3.grind_size_tenths;
        snprintf(e.grind_unit, sizeof(e.grind_unit), "%s", m.p3.grind_unit);
    }
    return e;
}

static GlinkResultInfo sample_result() {
    GlinkResultInfo r;
    r.seq = 5;
    r.code = GlinkResultCode::OK;
    r.origin = GlinkOrigin::BLE;
    r.profile = 3;
    r.mode = GlinkMode::WEIGHT;
    r.target_tenths = 180;
    r.final_weight_tenths = 181;
    r.weight_settled = true;
    r.pulses = 2;
    r.duration_ms = 14230;
    r.motor_ms = 11800;
    return r;
}

// The response to a request text in a model state ("" = no response).
static std::string respond(const std::string& text, const Model& m) {
    char out[GLINK_MAX_REPLY_BYTES + 1];
    GlinkRequest req;
    GlinkFailure f;
    if (!glink_parse_request(text.data(), text.size(), req, f) || !glink_gate(m.handshaken, m.pending, req, f)) {
        const size_t n = glink_encode_failure(out, sizeof(out), f);
        TEST_ASSERT_TRUE(n > 0);
        return std::string(out, n);
    }
    const int object = static_cast<int>(req.object);
    auto fail = [&](GlinkError code, const char* message, const char* field) {
        const size_t n = glink_encode_error(out, sizeof(out), object, code, message, field);
        TEST_ASSERT_TRUE(n > 0);
        return std::string(out, n);
    };
    auto failure = [&]() {
        const size_t n = glink_encode_failure(out, sizeof(out), f);
        TEST_ASSERT_TRUE(n > 0);
        return std::string(out, n);
    };
    size_t n = 0;
    if (req.op == GlinkOp::STOP) {
        n = glink_encode_stop(out, sizeof(out));
        return std::string(out, n);
    }
    if (req.op == GlinkOp::HANDSHAKE) return "";
    if (m.ui_busy) return fail(GlinkError::UI_BUSY, glink_msg::UI_BUSY, "");
    const bool interval_ok = glink_write_interval_ok(m.have_last, m.last_store, NOW);
    switch (req.op) {
        case GlinkOp::GET:
            if (!glink_state_check(m.screen, GlinkOpClass::GET, object, f)) return failure();
            if (req.object == GlinkObject::GRIND_RESULT) {
                if (!m.has_result) return fail(GlinkError::NOT_FOUND, glink_msg::NO_RESULT, "");
                n = glink_encode_result_get(out, sizeof(out), sample_result());
            } else if (req.selector_index) {
                if (req.selector_value == 3 && !m.p3.exists) {
                    return fail(GlinkError::NOT_FOUND, glink_msg::PROFILE_NOT_SET, "selector.value");
                }
                n = glink_encode_profile_entry(out, sizeof(out), entry_of(m, req.selector_value));
            } else {
                GlinkProfileEntry list[4];
                size_t count = 0;
                for (uint8_t i = 0; i < 3; i++) list[count++] = entry_of(m, i);
                if (m.p3.exists) list[count++] = entry_of(m, 3);
                n = glink_encode_profile_list(out, sizeof(out), list, count);
            }
            break;
        case GlinkOp::SELECT: {
            bool will_store = false;
            if (!glink_decide_select(m.screen, req.index, m.p3.exists, m.selected, interval_ok, &will_store, f)) return failure();
            n = glink_encode_select(out, sizeof(out), req.index);
            break;
        }
        case GlinkOp::UPDATE: {
            GlinkUpdatePlan plan;
            if (!glink_decide_update(m.screen, m.p3, req, interval_ok, plan, f)) return failure();
            if (plan.changed && m.store_fails) return fail(GlinkError::STORE_FAILED, glink_msg::STORE_FAILED, "");
            n = glink_encode_profile_update(out, sizeof(out), plan.result, plan.changed, plan.changed);
            break;
        }
        case GlinkOp::START: {
            GlinkStartInputs in;
            in.profile_exists = req.index < 3 || m.p3.exists;
            in.stored_dose_tenths = req.index < 3 ? m.dose[req.index] : m.p3.dose_tenths;
            in.has_expect_dose = req.has_expect_dose;
            in.expect_dose_tenths = req.expect_dose_tenths;
            in.block = m.block;
            if (!glink_decide_start(m.screen, in, f)) return failure();
            n = glink_encode_start(out, sizeof(out), GLINK_START_LEAD_IN_MS);
            break;
        }
        default: TEST_FAIL_MESSAGE("unexpected op");
    }
    TEST_ASSERT_TRUE(n > 0);
    return std::string(out, n);
}

static void test_pairs() {
    int checked = 0;
    for (const GJson& p : glink_vectors().at("pairs").a) {
        const char* name = p.at("name").s.c_str();
        const Model m = model_from_state(p.at("state").s);
        if (p.at("request").is_null()) continue;  // frame-level pair, see the frame tests
        const std::string got = respond(p.at("request").s, m);
        const GJson& response = p.at("response");
        if (response.is_null()) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE("", got.c_str(), name);
        } else {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(response.s.c_str(), got.c_str(), name);
        }
        checked++;
    }
    TEST_ASSERT_TRUE(checked >= 100);
}

static void test_pair_payload_too_large() {
    char out[512];
    for (const GJson& p : glink_vectors().at("pairs").a) {
        if (!p.at("request").is_null()) continue;
        const size_t n = glink_encode_error(out, sizeof(out), -1, GlinkError::PAYLOAD_TOO_LARGE, glink_msg::TOO_LARGE, "");
        TEST_ASSERT_EQUAL_STRING(p.at("response").s.c_str(), std::string(out, n).c_str());
    }
}

static void test_every_error_code_is_exercised_by_a_pair() {
    bool seen[static_cast<int>(GlinkError::COUNT)] = {};
    for (const GJson& p : glink_vectors().at("pairs").a) {
        const GJson& response = p.at("response");
        if (response.is_null()) continue;
        for (int i = 1; i < static_cast<int>(GlinkError::COUNT); i++) {
            const std::string needle = std::string("\"code\":\"") + glink_error_code(static_cast<GlinkError>(i)) + "\"";
            if (contains(response.s, needle.c_str())) seen[i] = true;
        }
    }
    for (int i = 1; i < static_cast<int>(GlinkError::COUNT); i++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[i], glink_error_code(static_cast<GlinkError>(i)));
    }
}

//------------------------------------------------------------------------------
// Broadcasts
//------------------------------------------------------------------------------
static void test_handshake_broadcasts() {
    for (const GJson& p : glink_vectors().at("pairs").a) {
        const GJson* b = p.find("broadcasts");
        if (!b) continue;
        GlinkBaseInfo info;
        info.release_ver = 640;
        info.sn_code = "7c2a3f10b4d8";
        info.fw_version = "1.4.1";
        info.board = "V2";
        char out[GLINK_MAX_REPLY_BYTES + 1];
        size_t n = glink_encode_base_info(out, sizeof(out), info);
        TEST_ASSERT_EQUAL_STRING(b->a[0].s.c_str(), std::string(out, n).c_str());
        GlinkPeriodInfo period;
        period.dev_state = GlinkDevState::IDLE;
        period.weight_valid = true;
        period.select_profile = 1;
        n = glink_encode_period_info(out, sizeof(out), period);
        TEST_ASSERT_EQUAL_STRING(b->a[1].s.c_str(), std::string(out, n).c_str());
    }
}

template <typename E>
static E enum_from_name(const char* (*name_of)(E), int count, const std::string& text, int first = 0) {
    for (int i = first; i < count; i++) {
        if (text == name_of(static_cast<E>(i))) return static_cast<E>(i);
    }
    TEST_FAIL_MESSAGE(text.c_str());
    return static_cast<E>(0);
}

static int32_t tenths_of_json(const GJson& v) { return static_cast<int32_t>(v.n * 10 + (v.n < 0 ? -0.5 : 0.5)); }

static GlinkPeriodInfo period_from_json(const GJson& o) {
    GlinkPeriodInfo p;
    p.dev_state = enum_from_name(glink_dev_state_name, static_cast<int>(GlinkDevState::COUNT), o.at("devState").s);
    p.weight_valid = !o.at("weight").is_null();
    if (p.weight_valid) p.weight_tenths = tenths_of_json(o.at("weight"));
    p.select_profile = static_cast<uint8_t>(o.at("selectProfile").n);
    if (const GJson* v = o.find("phase")) {
        p.phase = enum_from_name(glink_phase_name, static_cast<int>(GlinkPhase::COUNT), v->s, 1);
    }
    if (const GJson* v = o.find("target")) {
        p.has_target = true;
        p.target_tenths = tenths_of_json(*v);
        p.mode = enum_from_name(glink_mode_name, static_cast<int>(GlinkMode::COUNT), o.at("mode").s);
    }
    if (const GJson* v = o.find("leadInMs")) {
        p.has_lead_in = true;
        p.lead_in_ms = static_cast<uint32_t>(v->n);
    }
    if (!o.at("blocked").is_null()) {
        p.blocked = enum_from_name(glink_error_code, static_cast<int>(GlinkError::COUNT), o.at("blocked").s, 1);
    }
    p.result_seq = static_cast<uint32_t>(o.at("resultSeq").n);
    p.profile_rev = static_cast<uint32_t>(o.at("profileRev").n);
    return p;
}

static void test_period_cases() {
    for (const GJson& c : glink_vectors().at("period_cases").a) {
        const GJson msg = glink_json_parse_text(c.at("message").s);
        const GlinkPeriodInfo p = period_from_json(msg.at("broadcast").at("periodInfo"));
        char out[GLINK_MAX_REPLY_BYTES + 1];
        const size_t n = glink_encode_period_info(out, sizeof(out), p);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.at("message").s.c_str(), std::string(out, n).c_str(), c.at("name").s.c_str());
    }
}

static void test_period_blocked_follows_dev_state() {
    // 6.5: the blocked value of every period case is what the rule table says for its devState.
    for (const GJson& c : glink_vectors().at("period_cases").a) {
        const GJson msg = glink_json_parse_text(c.at("message").s);
        const GlinkPeriodInfo p = period_from_json(msg.at("broadcast").at("periodInfo"));
        // For WARNING the start block is the blocked value itself.
        GlinkStartBlock block = GlinkStartBlock::NONE;
        switch (p.blocked) {
            case GlinkError::MOTOR_DISABLED: block = GlinkStartBlock::MOTOR_DISABLED; break;
            case GlinkError::UPDATING: block = GlinkStartBlock::UPDATE_LOCK; break;
            case GlinkError::LOAD_CELL_NOT_CONNECTED: block = GlinkStartBlock::LOAD_CELL_NOT_CONNECTED; break;
            case GlinkError::LOAD_CELL_NO_DATA: block = GlinkStartBlock::LOAD_CELL_NO_DATA; break;
            case GlinkError::LOAD_CELL_RATE_INVALID: block = GlinkStartBlock::LOAD_CELL_RATE_INVALID; break;
            default: break;
        }
        TEST_ASSERT_EQUAL_MESSAGE(p.blocked, glink_blocked(p.dev_state, block), c.at("name").s.c_str());
    }
}

static void test_result_broadcasts_and_get() {
    for (const GJson& c : glink_vectors().at("result_broadcasts").a) {
        const GJson msg = glink_json_parse_text(c.at("message").s);
        const GJson& o = msg.at("broadcast").at("grindResult");
        GlinkResultInfo r;
        r.seq = static_cast<uint32_t>(o.at("seq").n);
        r.code = enum_from_name(glink_result_code_name, static_cast<int>(GlinkResultCode::COUNT), o.at("code").s);
        r.origin = enum_from_name(glink_origin_name, static_cast<int>(GlinkOrigin::COUNT), o.at("origin").s);
        r.profile = static_cast<uint8_t>(o.at("profile").n);
        r.mode = enum_from_name(glink_mode_name, static_cast<int>(GlinkMode::COUNT), o.at("mode").s);
        r.target_tenths = tenths_of_json(o.at("target"));
        r.final_weight_tenths = tenths_of_json(o.at("finalWeight"));
        r.weight_settled = o.at("weightSettled").b;
        r.pulses = static_cast<uint32_t>(o.at("pulses").n);
        r.duration_ms = static_cast<uint32_t>(o.at("durationMs").n);
        r.motor_ms = static_cast<uint32_t>(o.at("motorMs").n);
        char out[GLINK_MAX_REPLY_BYTES + 1];
        const size_t n = glink_encode_result_broadcast(out, sizeof(out), r);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.at("message").s.c_str(), std::string(out, n).c_str(), c.at("name").s.c_str());
    }
}

static void test_encoder_capacity() {
    GlinkPeriodInfo p;
    p.dev_state = GlinkDevState::GRINDING;
    p.phase = GlinkPhase::PREDICTIVE;
    p.has_target = true;
    char full[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_period_info(full, sizeof(full), p);
    TEST_ASSERT_TRUE(n > 0);
    // One byte short of the NUL-terminated size: zero, and never beyond the buffer.
    for (size_t cap = 0; cap <= n; cap++) {
        char arena[GLINK_MAX_REPLY_BYTES + 32];
        memset(arena, 0x77, sizeof(arena));
        TEST_ASSERT_EQUAL_UINT32(0, glink_encode_period_info(arena + 16, cap, p));
        for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX8(0x77, static_cast<unsigned char>(arena[i]));
        for (size_t i = cap + 16; i < sizeof(arena); i++) TEST_ASSERT_EQUAL_HEX8(0x77, static_cast<unsigned char>(arena[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(n, glink_encode_period_info(full, n + 1, p));
}

static void test_profile_list_fits_the_reply_limit() {
    GlinkProfileEntry list[4];
    for (uint8_t i = 0; i < 4; i++) {
        list[i].index = i;
        list[i].dose_tenths = 9999;
        list[i].has_grind_size = true;
        list[i].grind_size_tenths = 9999;
        snprintf(list[i].grind_unit, sizeof(list[i].grind_unit), "%s", "clicks");
    }
    char out[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_profile_list(out, sizeof(out), list, 4);
    TEST_ASSERT_TRUE(n > 0 && n <= GLINK_MAX_REPLY_BYTES);
}

static void test_unit_without_size_is_never_written() {
    GlinkProfileEntry e;
    e.index = 3;
    e.dose_tenths = 180;
    snprintf(e.grind_unit, sizeof(e.grind_unit), "%s", "um");
    char out[GLINK_MAX_REPLY_BYTES + 1];
    const size_t n = glink_encode_profile_entry(out, sizeof(out), e);
    TEST_ASSERT_TRUE(contains(std::string(out, n), "\"grindSize\":null,\"grindUnit\":null"));
}

void run_glink_messages_tests() {
    RUN_TEST(test_vocabularies_match_vectors);
    RUN_TEST(test_derived_limits_match_constants);
    RUN_TEST(test_dose_cases);
    RUN_TEST(test_grind_size_cases);
    RUN_TEST(test_grind_unit_cases);
    RUN_TEST(test_expect_dose_uses_the_dose_rules);
    RUN_TEST(test_oversize_request_is_payload_too_large);
    RUN_TEST(test_parsed_fields);
    RUN_TEST(test_failures_carry_the_object);
    RUN_TEST(test_op_variants);
    RUN_TEST(test_envelope_order);
    RUN_TEST(test_field_path_is_bounded);
    RUN_TEST(test_pairs);
    RUN_TEST(test_pair_payload_too_large);
    RUN_TEST(test_every_error_code_is_exercised_by_a_pair);
    RUN_TEST(test_handshake_broadcasts);
    RUN_TEST(test_period_cases);
    RUN_TEST(test_period_blocked_follows_dev_state);
    RUN_TEST(test_result_broadcasts_and_get);
    RUN_TEST(test_encoder_capacity);
    RUN_TEST(test_profile_list_fits_the_reply_limit);
    RUN_TEST(test_unit_without_size_is_never_written);
}
