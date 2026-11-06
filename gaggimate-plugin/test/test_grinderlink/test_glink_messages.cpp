// Client side messages against the shared conformance vectors (tools/tests/glink_vectors.json): request builders
// reproduce the vector requests byte for byte, every response and broadcast of the vectors parses to the values the
// vector JSON holds.
#include <math.h>
#include <unity.h>
#include <string>
#include "GLinkMessages.h"
#include "glink_vectors.h"

using namespace glinkc;

namespace {

int32_t tenths_of(const GJson& v) { return static_cast<int32_t>(llround(v.n * 10.0)); }

const GJson& pair_named(const char* name) {
    for (const GJson& p : glink_vectors().at("pairs").a) {
        if (p.at("name").s == name) return p;
    }
    TEST_FAIL_MESSAGE(name);
    return glink_vectors();
}

std::string request_of(const char* name) { return pair_named(name).at("request").s; }

void expect_request(const char* pair, size_t built, const char* out) {
    TEST_ASSERT_TRUE_MESSAGE(built > 0, pair);
    const std::string want = request_of(pair);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), out, pair);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(strlen(out)), static_cast<long long>(built), pair);
}

}  // namespace

void test_builders_reproduce_vector_requests() {
    char out[GLINK_MAX_REQUEST_BYTES];
    expect_request("appHello_handshake", build_hello(out, sizeof(out)), out);
    expect_request("profile_get_all_without_profile_3", build_profile_get(out, sizeof(out), -1), out);
    expect_request("profile_get_one", build_profile_get(out, sizeof(out), 3), out);
    expect_request("profile_select", build_profile_select(out, sizeof(out), 2), out);

    ProfileUpdate u;
    u.has_dose = true; u.dose_tenths = 180;
    u.has_size = true; u.size_tenths = 145;
    u.has_unit = true; strcpy(u.unit, "um");
    expect_request("profile_update_create", build_profile_update(out, sizeof(out), 3, u), out);

    ProfileUpdate dose_only;
    dose_only.has_dose = true; dose_only.dose_tenths = 185;
    expect_request("profile_update_dose_only", build_profile_update(out, sizeof(out), 3, dose_only), out);

    ProfileUpdate clear_size;
    clear_size.has_size = true; clear_size.size_null = true;
    expect_request("profile_update_clear_size", build_profile_update(out, sizeof(out), 3, clear_size), out);

    ProfileUpdate clear_unit;
    clear_unit.has_unit = true; clear_unit.unit_null = true;
    expect_request("profile_update_clear_unit_only", build_profile_update(out, sizeof(out), 3, clear_unit), out);

    expect_request("control_start", build_start(out, sizeof(out), 3, true, 180), out);
    expect_request("control_start_without_expect", build_start(out, sizeof(out), 1, false, 0), out);
    expect_request("control_stop", build_stop(out, sizeof(out)), out);
    expect_request("result_get", build_result_get(out, sizeof(out)), out);
}

void test_builders_refuse_what_the_grinder_refuses() {
    char out[GLINK_MAX_REQUEST_BYTES];
    TEST_ASSERT_EQUAL_INT(0, build_profile_get(out, sizeof(out), 4));
    TEST_ASSERT_EQUAL_INT(0, build_profile_select(out, sizeof(out), -1));
    TEST_ASSERT_EQUAL_INT(0, build_profile_select(out, sizeof(out), 4));
    TEST_ASSERT_EQUAL_INT(0, build_start(out, sizeof(out), 4, false, 0));
    TEST_ASSERT_EQUAL_INT(0, build_start(out, sizeof(out), 3, true, 49));
    TEST_ASSERT_EQUAL_INT(0, build_start(out, sizeof(out), 3, true, 601));
    TEST_ASSERT_TRUE(build_start(out, sizeof(out), 3, true, 50) > 0);
    TEST_ASSERT_TRUE(build_start(out, sizeof(out), 3, true, 600) > 0);
    TEST_ASSERT_EQUAL_INT(0, build_hello(out, 10));  // does not fit

    ProfileUpdate u;
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, u));  // nothing to update
    u.has_dose = true; u.dose_tenths = 49;
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, u));
    u.dose_tenths = 601;
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, u));
    u.dose_tenths = 180;
    TEST_ASSERT_TRUE(build_profile_update(out, sizeof(out), 3, u) > 0);
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 4, u));
    u.has_size = true; u.size_tenths = 10000;
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, u));
    u.size_tenths = -1;
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, u));
    u.size_tenths = 9999;
    TEST_ASSERT_TRUE(build_profile_update(out, sizeof(out), 3, u) > 0);

    // a unit needs a size in the same request
    ProfileUpdate unit_only;
    unit_only.has_unit = true; strcpy(unit_only.unit, "um");
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, unit_only));
    ProfileUpdate unit_with_null_size;
    unit_with_null_size.has_size = true; unit_with_null_size.size_null = true;
    unit_with_null_size.has_unit = true; strcpy(unit_with_null_size.unit, "um");
    TEST_ASSERT_EQUAL_INT(0, build_profile_update(out, sizeof(out), 3, unit_with_null_size));
}

void test_unit_charset_and_length() {
    TEST_ASSERT_TRUE(unit_ok("um"));
    TEST_ASSERT_TRUE(unit_ok(""));
    TEST_ASSERT_TRUE(unit_ok("a.b/c%-_"[0] ? "a.b/c" : ""));  // 5 characters from the allowed set
    TEST_ASSERT_TRUE(unit_ok("clicks"));                      // 6 characters
    TEST_ASSERT_FALSE(unit_ok("abcdefg"));                    // 7
    TEST_ASSERT_FALSE(unit_ok("a;b"));
    TEST_ASSERT_FALSE(unit_ok("a\"b"));
    TEST_ASSERT_FALSE(unit_ok("a\\b"));
    TEST_ASSERT_FALSE(unit_ok("\xc3\xa5"));  // non-ASCII
    TEST_ASSERT_FALSE(unit_ok(" um"));
    TEST_ASSERT_FALSE(unit_ok("um "));
    TEST_ASSERT_FALSE(unit_ok(nullptr));

    // The unit vectors the grinder accepts as a plain label must be accepted by the client check too.
    for (const GJson& c : glink_vectors().at("grind_unit_cases").a) {
        const GJson& expect = c.at("expect");
        if (!expect.at("ok").b || expect.at("value").is_null()) continue;
        TEST_ASSERT_TRUE_MESSAGE(unit_ok(expect.at("value").s.c_str()), c.at("name").s.c_str());
    }
}

void test_dose_range_matches_vectors() {
    for (const GJson& c : glink_vectors().at("dose_cases").a) {
        const GJson& expect = c.at("expect");
        if (!expect.at("ok").b) continue;
        const GJson num = glink_json_parse_text(c.at("json").s);
        if (num.kind != GJson::NUM) continue;
        // valid vector doses, as the grinder stores them (one decimal), are inside the client's range
        const int32_t t = static_cast<int32_t>(llround(atof(expect.at("value").s.c_str()) * 10.0));
        TEST_ASSERT_TRUE_MESSAGE(dose_ok(t), c.at("name").s.c_str());
    }
    TEST_ASSERT_FALSE(dose_ok(49));
    TEST_ASSERT_TRUE(dose_ok(50));
    TEST_ASSERT_TRUE(dose_ok(600));
    TEST_ASSERT_FALSE(dose_ok(601));
}

namespace {

void check_response_against_vector(const std::string& text, const std::string& label) {
    const GJson doc = glink_json_parse_text(text);
    const GJson& body = doc.at("response");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, static_cast<long long>(body.o.size()), label.c_str());
    const std::string& object = body.o[0].first;
    const GJson& r = body.o[0].second;

    static Message m;
    TEST_ASSERT_TRUE_MESSAGE(parse_message(text.data(), text.size(), m), label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Kind::RESPONSE), static_cast<int>(m.kind), label.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(object.c_str(), m.response.object, label.c_str());

    if (r.at("result").s == "fail") {
        const GJson& e = r.at("error");
        TEST_ASSERT_FALSE_MESSAGE(m.response.success, label.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(e.at("code").s.c_str(), m.response.failure.code, label.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(e.at("message").s.c_str(), m.response.failure.message, label.c_str());
        const GJson* field = e.find("field");
        TEST_ASSERT_EQUAL_STRING_MESSAGE(field ? field->s.c_str() : "", m.response.failure.field, label.c_str());
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(m.response.success, label.c_str());
    const GJson* data = r.find("data");
    if (data == nullptr) return;
    if (data->kind == GJson::ARR) {
        TEST_ASSERT_TRUE_MESSAGE(m.response.entries_are_list, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(data->a.size()), m.response.entry_count, label.c_str());
        for (size_t i = 0; i < data->a.size(); i++) {
            const GJson& e = data->a[i];
            const ProfileEntry& got = m.response.entries[i];
            TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(e.at("index").n), got.index, label.c_str());
            TEST_ASSERT_EQUAL_STRING_MESSAGE(e.at("name").s.c_str(), got.name, label.c_str());
            TEST_ASSERT_EQUAL_INT_MESSAGE(tenths_of(e.at("dose")), got.dose_tenths, label.c_str());
            TEST_ASSERT_EQUAL_INT_MESSAGE(e.at("grindSize").is_null() ? 0 : 1, got.has_grind_size ? 1 : 0, label.c_str());
            if (!e.at("grindSize").is_null()) TEST_ASSERT_EQUAL_INT_MESSAGE(tenths_of(e.at("grindSize")), got.grind_size_tenths, label.c_str());
            TEST_ASSERT_EQUAL_STRING_MESSAGE(e.at("grindUnit").is_null() ? "" : e.at("grindUnit").s.c_str(), got.grind_unit, label.c_str());
            TEST_ASSERT_EQUAL_INT_MESSAGE(e.at("writable").b ? 1 : 0, got.writable ? 1 : 0, label.c_str());
        }
        return;
    }
    if (const GJson* v = data->find("selectProfile")) {
        TEST_ASSERT_TRUE_MESSAGE(m.response.has_select, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(v->n), m.response.select_profile, label.c_str());
    } else if (const GJson* v = data->find("leadInMs")) {
        TEST_ASSERT_TRUE_MESSAGE(m.response.has_lead_in, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(v->n), m.response.lead_in_ms, label.c_str());
    } else if (const GJson* v = data->find("accepted")) {
        TEST_ASSERT_TRUE_MESSAGE(m.response.has_accepted, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(v->b ? 1 : 0, m.response.accepted ? 1 : 0, label.c_str());
    } else if (data->find("seq") != nullptr) {
        TEST_ASSERT_TRUE_MESSAGE(m.response.has_result, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(data->at("seq").n), m.response.result.seq, label.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(data->at("code").s.c_str(), m.response.result.code, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(tenths_of(data->at("finalWeight")), m.response.result.final_weight_tenths, label.c_str());
    } else if (data->find("index") != nullptr) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, m.response.entry_count, label.c_str());
        TEST_ASSERT_FALSE_MESSAGE(m.response.entries_are_list, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(data->at("index").n), m.response.entries[0].index, label.c_str());
        TEST_ASSERT_EQUAL_INT_MESSAGE(tenths_of(data->at("dose")), m.response.entries[0].dose_tenths, label.c_str());
        if (const GJson* changed = data->find("changed")) {
            TEST_ASSERT_TRUE_MESSAGE(m.response.has_update_flags, label.c_str());
            TEST_ASSERT_EQUAL_INT_MESSAGE(changed->b ? 1 : 0, m.response.changed ? 1 : 0, label.c_str());
            TEST_ASSERT_EQUAL_INT_MESSAGE(data->at("stored").b ? 1 : 0, m.response.stored ? 1 : 0, label.c_str());
        }
    }
}

void check_period_against_vector(const std::string& text, const std::string& label) {
    const GJson doc = glink_json_parse_text(text);
    const GJson& p = doc.at("broadcast").at("periodInfo");
    static Message m;
    TEST_ASSERT_TRUE_MESSAGE(parse_message(text.data(), text.size(), m), label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Kind::PERIOD_INFO), static_cast<int>(m.kind), label.c_str());
    const PeriodInfo& g = m.period;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(p.at("devState").s.c_str(), g.dev_state, label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(p.at("weight").is_null() ? 0 : 1, g.weight_valid ? 1 : 0, label.c_str());
    if (!p.at("weight").is_null()) TEST_ASSERT_EQUAL_INT_MESSAGE(tenths_of(p.at("weight")), g.weight_tenths, label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(p.at("selectProfile").n), g.select_profile, label.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(p.find("phase") ? p.at("phase").s.c_str() : "", g.phase, label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(p.find("target") ? 1 : 0, g.has_target ? 1 : 0, label.c_str());
    if (p.find("target")) TEST_ASSERT_EQUAL_INT_MESSAGE(tenths_of(p.at("target")), g.target_tenths, label.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(p.find("mode") ? p.at("mode").s.c_str() : "", g.mode, label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(p.find("leadInMs") ? 1 : 0, g.has_lead_in ? 1 : 0, label.c_str());
    if (p.find("leadInMs")) TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(p.at("leadInMs").n), g.lead_in_ms, label.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(p.at("blocked").is_null() ? "" : p.at("blocked").s.c_str(), g.blocked, label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(p.at("resultSeq").n), g.result_seq, label.c_str());
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<long long>(p.at("profileRev").n), g.profile_rev, label.c_str());
}

}  // namespace

void test_every_vector_response_parses() {
    int checked = 0;
    for (const GJson& p : glink_vectors().at("pairs").a) {
        const GJson& r = p.at("response");
        if (r.is_null()) continue;
        check_response_against_vector(r.s, p.at("name").s);
        checked++;
    }
    TEST_ASSERT_TRUE(checked > 80);
}

void test_every_vector_broadcast_parses() {
    int periods = 0;
    for (const GJson& p : glink_vectors().at("pairs").a) {
        const GJson* b = p.find("broadcasts");
        if (b == nullptr) continue;
        for (const GJson& text : b->a) {
            static Message m;
            const GJson bdoc = glink_json_parse_text(text.s);
            const GJson& inner = bdoc.at("broadcast");
            if (inner.find("periodInfo")) {
                check_period_against_vector(text.s, p.at("name").s);
                periods++;
            } else if (inner.find("baseInfo")) {
                const GJson& bi = inner.at("baseInfo");
                TEST_ASSERT_TRUE(parse_message(text.s.data(), text.s.size(), m));
                TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::BASE_INFO), static_cast<int>(m.kind));
                TEST_ASSERT_EQUAL_STRING(bi.at("snCode").s.c_str(), m.base.sn_code);
                TEST_ASSERT_EQUAL_STRING("SGBW", m.base.device);
                TEST_ASSERT_EQUAL_INT(1, m.base.proto);
                TEST_ASSERT_EQUAL_STRING(bi.at("fwVersion").s.c_str(), m.base.fw_version);
                TEST_ASSERT_EQUAL_STRING(bi.at("board").s.c_str(), m.base.board);
                TEST_ASSERT_EQUAL_INT(50, m.base.dose_min_tenths);
                TEST_ASSERT_EQUAL_INT(600, m.base.dose_max_tenths);
                TEST_ASSERT_EQUAL_INT(512, m.base.max_request_bytes);
                TEST_ASSERT_EQUAL_INT(static_cast<long long>(bi.at("releaseVer").n), m.base.release_ver);
            }
        }
    }
    TEST_ASSERT_TRUE(periods >= 1);
}

void test_period_cases_parse() {
    for (const GJson& c : glink_vectors().at("period_cases").a) check_period_against_vector(c.at("message").s, c.at("name").s);
}

void test_result_broadcasts_parse() {
    for (const GJson& c : glink_vectors().at("result_broadcasts").a) {
        const std::string& text = c.at("message").s;
        const GJson rdoc = glink_json_parse_text(text);
        const GJson& r = rdoc.at("broadcast").at("grindResult");
        static Message m;
        TEST_ASSERT_TRUE(parse_message(text.data(), text.size(), m));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::GRIND_RESULT), static_cast<int>(m.kind));
        TEST_ASSERT_EQUAL_INT(static_cast<long long>(r.at("seq").n), m.result.seq);
        TEST_ASSERT_EQUAL_STRING(r.at("code").s.c_str(), m.result.code);
        TEST_ASSERT_EQUAL_STRING(r.at("origin").s.c_str(), m.result.origin);
        TEST_ASSERT_EQUAL_INT(static_cast<long long>(r.at("profile").n), m.result.profile);
        TEST_ASSERT_EQUAL_STRING(r.at("mode").s.c_str(), m.result.mode);
        TEST_ASSERT_EQUAL_INT(tenths_of(r.at("target")), m.result.target_tenths);
        TEST_ASSERT_EQUAL_INT(tenths_of(r.at("finalWeight")), m.result.final_weight_tenths);
        TEST_ASSERT_EQUAL_INT(r.at("weightSettled").b ? 1 : 0, m.result.weight_settled ? 1 : 0);
        TEST_ASSERT_EQUAL_INT(static_cast<long long>(r.at("pulses").n), m.result.pulses);
        TEST_ASSERT_EQUAL_INT(static_cast<long long>(r.at("durationMs").n), m.result.duration_ms);
        TEST_ASSERT_EQUAL_INT(static_cast<long long>(r.at("motorMs").n), m.result.motor_ms);
    }
}

void test_unknown_vocabulary_is_tolerated() {
    const char* text =
        "{\"broadcast\":{\"periodInfo\":{\"devState\":\"FUTURE_STATE\",\"weight\":-0.3,\"selectProfile\":0,\"phase\":\"NEW_PHASE\","
        "\"blocked\":\"SOMETHING_NEW\",\"resultSeq\":0,\"profileRev\":0,\"extra\":{\"a\":1}}}}";
    static Message m;
    TEST_ASSERT_TRUE(parse_message(text, strlen(text), m));
    TEST_ASSERT_EQUAL_STRING("FUTURE_STATE", m.period.dev_state);
    TEST_ASSERT_EQUAL_STRING("NEW_PHASE", m.period.phase);
    TEST_ASSERT_EQUAL_STRING("SOMETHING_NEW", m.period.blocked);
    TEST_ASSERT_EQUAL_INT(-3, m.period.weight_tenths);

    const char* result =
        "{\"broadcast\":{\"grindResult\":{\"seq\":9,\"code\":\"NEW_CODE\",\"origin\":\"ROBOT\",\"profile\":1,\"mode\":\"WEIGHT\","
        "\"target\":18.0,\"finalWeight\":18.0,\"weightSettled\":true,\"pulses\":0,\"durationMs\":1,\"motorMs\":1}}}";
    TEST_ASSERT_TRUE(parse_message(result, strlen(result), m));
    TEST_ASSERT_EQUAL_STRING("NEW_CODE", m.result.code);
}

void test_garbage_is_refused() {
    static Message m;
    const char* bad[] = {"", "{", "{}", "[]", "{\"request\":{}}", "{\"broadcast\":{}}", "{\"broadcast\":{\"periodInfo\":{}}}",
                         "{\"broadcast\":{\"periodInfo\":{\"devState\":\"\"}}}", "{\"broadcast\":{\"nope\":{}}}",
                         "{\"response\":{\"grindProfile\":{\"result\":\"maybe\"}}}",
                         "{\"response\":{\"grindProfile\":{\"result\":\"fail\"}}}",
                         "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":[{\"index\":9}]}}}",
                         "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":[{\"index\":1,\"dose\":18.0}"};
    for (const char* t : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parse_message(t, strlen(t), m), t);
    }
    TEST_ASSERT_FALSE(parse_message(nullptr, 5, m));
    char big[GLINK_MAX_REPLY_BYTES + 8];
    memset(big, ' ', sizeof(big));
    TEST_ASSERT_FALSE(parse_message(big, sizeof(big), m));
}

void test_hostile_input_is_refused_without_overrun() {
    static Message m;
    const char* bad[] = {
        // unterminated message text and unterminated array
        "{\"response\":{\"grindProfile\":{\"result\":\"fail\",\"error\":{\"code\":\"X\",\"message\":\"never ends",
        "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":[{\"index\":1,\"dose\":18.0},",
        // an escaped quote inside a message
        "{\"response\":{\"grindProfile\":{\"result\":\"fail\",\"error\":{\"code\":\"X\",\"message\":\"a\\\"b\"}}}}",
        // an array marker inside a periodInfo must not be taken for a profile list
        "{\"broadcast\":{\"periodInfo\":{\"devState\":\"IDLE\",\"data\":[{\"index\":1,\"dose\":18.0}],\"selectProfile\":1}}}",
        // a nested object or array inside an entry
        "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":[{\"index\":1,\"dose\":18.0,\"x\":{\"a\":1}}]}}}",
        "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":[{\"index\":1,\"dose\":18.0,\"x\":[1]}]}}}",
        // more entries than profiles
        "{\"response\":{\"grindProfile\":{\"result\":\"success\",\"data\":[{\"index\":0,\"dose\":1.0},{\"index\":1,\"dose\":1.0},"
        "{\"index\":2,\"dose\":1.0},{\"index\":3,\"dose\":1.0},{\"index\":3,\"dose\":1.0}]}}}"};
    for (const char* t : bad) {
        TEST_ASSERT_FALSE_MESSAGE(parse_message(t, strlen(t), m), t);
    }
}

void test_retryable_failures() {
    TEST_ASSERT_TRUE(failure_is_retryable("BUSY"));
    TEST_ASSERT_TRUE(failure_is_retryable("UI_BUSY"));
    TEST_ASSERT_TRUE(failure_is_retryable("RATE_LIMITED"));
    TEST_ASSERT_FALSE(failure_is_retryable("OUT_OF_RANGE"));
    TEST_ASSERT_FALSE(failure_is_retryable(nullptr));
}

void run_glink_plugin_messages_tests() {
    RUN_TEST(test_builders_reproduce_vector_requests);
    RUN_TEST(test_builders_refuse_what_the_grinder_refuses);
    RUN_TEST(test_unit_charset_and_length);
    RUN_TEST(test_dose_range_matches_vectors);
    RUN_TEST(test_every_vector_response_parses);
    RUN_TEST(test_every_vector_broadcast_parses);
    RUN_TEST(test_period_cases_parse);
    RUN_TEST(test_result_broadcasts_parse);
    RUN_TEST(test_unknown_vocabulary_is_tolerated);
    RUN_TEST(test_garbage_is_refused);
    RUN_TEST(test_hostile_input_is_refused_without_overrun);
    RUN_TEST(test_retryable_failures);
}
