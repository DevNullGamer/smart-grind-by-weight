// Web panel view of the history (chg016 step 11): the copy the UI task publishes
#include <unity.h>
#include <math.h>
#include <string.h>
#include "logging/grind_history.h"
#include "network/portal_api.h"
#include "network/portal_insights.h"
#include "portal_vectors.h"

static void test_history_snapshot_converts_field_for_field() {
    GrindHistorySnapshot snap;
    snap.sequence = 42;
    snap.loaded = true;
    snap.persistent = true;
    snap.count = 2;
    snap.entries[0].session_id = 9;
    snap.entries[0].ended_s = 120;
    snap.entries[0].time_ms = 14230;
    snap.entries[0].motor_ms = 9800;
    snap.entries[0].target = 18.0f;
    snap.entries[0].final_g = 18.1f;
    snap.entries[0].profile = 1;
    snap.entries[0].mode = 0;
    snap.entries[0].pulses = 2;
    snap.entries[0].result = 1;
    snap.entries[0].earlier_boot = false;
    snap.entries[1].session_id = 8;
    snap.entries[1].mode = 1;
    snap.entries[1].earlier_boot = true;

    PortalHistory out;
    portal_history_from_snapshot(snap, &out);
    TEST_ASSERT_EQUAL_UINT32(42, out.sequence);
    TEST_ASSERT_TRUE(out.loaded);
    TEST_ASSERT_TRUE(out.persistent);
    TEST_ASSERT_EQUAL_UINT8(2, out.count);
    TEST_ASSERT_EQUAL_UINT32(9, out.entries[0].id);
    TEST_ASSERT_EQUAL_UINT32(120, out.entries[0].ended_s);
    TEST_ASSERT_EQUAL_UINT32(14230, out.entries[0].time_ms);
    TEST_ASSERT_EQUAL_UINT32(9800, out.entries[0].motor_ms);
    TEST_ASSERT_EQUAL_FLOAT(18.0f, out.entries[0].target);
    TEST_ASSERT_EQUAL_FLOAT(18.1f, out.entries[0].final_g);
    TEST_ASSERT_EQUAL_UINT8(1, out.entries[0].profile);
    TEST_ASSERT_EQUAL_UINT8(2, out.entries[0].pulses);
    TEST_ASSERT_EQUAL_UINT8(1, out.entries[0].result);
    TEST_ASSERT_FALSE(out.entries[0].earlier_boot);
    TEST_ASSERT_EQUAL_UINT8(1, out.entries[1].mode);
    TEST_ASSERT_TRUE(out.entries[1].earlier_boot);
}

static void test_history_snapshot_not_loaded_and_empty() {
    GrindHistorySnapshot snap;  // never loaded: the page must not take this for an empty history
    PortalHistory out;
    out.count = 5;
    portal_history_from_snapshot(snap, &out);
    TEST_ASSERT_FALSE(out.loaded);
    TEST_ASSERT_FALSE(out.persistent);
    TEST_ASSERT_EQUAL_UINT8(0, out.count);
    portal_history_from_snapshot(snap, nullptr);  // no crash
}

static void test_history_snapshot_count_is_bounded() {
    GrindHistorySnapshot snap;
    snap.count = 200;  // a corrupt count never reads past the entries
    PortalHistory out;
    portal_history_from_snapshot(snap, &out);
    TEST_ASSERT_EQUAL_UINT8(PORTAL_HISTORY_MAX, out.count);
}

static void test_history_limits_agree() {
    TEST_ASSERT_EQUAL(PORTAL_HISTORY_MAX, GRIND_HISTORY_RING_CAPACITY);
    TEST_ASSERT_EQUAL(10, PORTAL_HISTORY_MAX);
}

// ---- GET /api/history encoder (chg016 step 12) ----

static float history_float(const PJson& v) {
    if (v.kind == PJson::STR) return v.s == "nan" ? NAN : (v.s == "inf" ? INFINITY : -INFINITY);
    return static_cast<float>(v.n);
}

static PortalHistory history_from_vector(const PJson& in, uint32_t* now_s) {
    PortalHistory h;
    *now_s = static_cast<uint32_t>(in.at("now_s").n);
    h.loaded = true;
    h.persistent = in.at("persistent").b;
    const PJson& entries = in.at("entries");
    TEST_ASSERT_TRUE(entries.a.size() <= PORTAL_HISTORY_MAX);
    for (const PJson& e : entries.a) {
        PortalHistoryEntry& p = h.entries[h.count++];
        p.id = static_cast<uint32_t>(e.at("id").n);
        p.ended_s = static_cast<uint32_t>(e.at("ended_s").n);
        p.earlier_boot = e.at("earlier_boot").b;
        p.profile = static_cast<uint8_t>(e.at("profile").n);
        p.mode = static_cast<uint8_t>(e.at("mode").n);
        p.target = history_float(e.at("target"));
        p.final_g = history_float(e.at("final_g"));
        p.time_ms = static_cast<uint32_t>(e.at("time_ms").n);
        p.motor_ms = static_cast<uint32_t>(e.at("motor_ms").n);
        p.pulses = static_cast<uint8_t>(e.at("pulses").n);
        p.result = static_cast<uint8_t>(e.at("result").n);
    }
    return h;
}

static PortalHistory history_at_every_maximum() {
    PortalHistory h;
    h.loaded = h.persistent = true;
    for (size_t i = 0; i < PORTAL_HISTORY_MAX; i++) {
        PortalHistoryEntry& e = h.entries[h.count++];
        e.id = 4294967295u - static_cast<uint32_t>(i);
        e.ended_s = 0;
        e.time_ms = e.motor_ms = 4294967295u;
        e.target = 3.0e38f;
        e.final_g = -3.0e38f;
        e.profile = 2;
        e.pulses = 255;
        e.result = static_cast<uint8_t>(GrindHistoryResult::MAX_PULSES);
    }
    return h;
}

static void test_history_vectors() {
    const PJson& cases = portal_vectors().at("history_cases");
    TEST_ASSERT_TRUE(cases.a.size() >= 10);
    for (const PJson& c : cases.a) {
        const std::string& name = c.at("name").s;
        uint32_t now_s = 0;
        const PortalHistory h = history_from_vector(c.at("input"), &now_s);
        char buf[PORTAL_HISTORY_JSON_MAX];
        TEST_ASSERT_TRUE_MESSAGE(portal_encode_history(h, now_s, buf, sizeof(buf)), name.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(c.at("json").s.c_str(), buf, name.c_str());
        const PJson j = portal_json_parse(buf);
        TEST_ASSERT_EQUAL_MESSAGE(h.count, static_cast<int>(j.at("count").n), name.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(h.count, j.at("grinds").a.size(), name.c_str());
    }
}

static void test_history_empty_is_the_documented_reply() {
    PortalHistory h;
    h.loaded = h.persistent = true;
    char buf[PORTAL_HISTORY_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_history(h, 5, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("{\"count\":0,\"persistent\":true,\"grinds\":[]}", buf);
}

static void test_history_worst_case_fits_its_buffer_with_margin() {
    char buf[PORTAL_HISTORY_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_history(history_at_every_maximum(), 4294967295u, buf, sizeof(buf)));
    const PJson j = portal_json_parse(buf);
    TEST_ASSERT_EQUAL(PORTAL_HISTORY_MAX, j.at("grinds").a.size());
    TEST_ASSERT_TRUE(j.at("grinds").a[0].at("target_g").n == PORTAL_HISTORY_WEIGHT_MAX);
    TEST_ASSERT_TRUE(j.at("grinds").a[0].at("final_g").n == -PORTAL_HISTORY_WEIGHT_MAX);
    TEST_ASSERT_TRUE(strlen(buf) + 300 < PORTAL_HISTORY_JSON_MAX);  // PORTAL-API.md documents the longest reply
}

static void test_history_truncation_never_yields_a_complete_reply() {
    const PortalHistory h = history_at_every_maximum();
    char full[PORTAL_HISTORY_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_history(h, 4294967295u, full, sizeof(full)));
    const size_t full_len = strlen(full);
    char buf[PORTAL_HISTORY_JSON_MAX];
    for (size_t cap = 1; cap <= full_len; cap++) {  // every cut, including inside a number or a name
        memset(buf, 'x', sizeof(buf));
        TEST_ASSERT_FALSE(portal_encode_history(h, 4294967295u, buf, cap));
        TEST_ASSERT_EQUAL_UINT32(cap - 1, strlen(buf));
    }
    TEST_ASSERT_TRUE(portal_encode_history(h, 4294967295u, buf, full_len + 1));
    TEST_ASSERT_FALSE(portal_encode_history(h, 4294967295u, buf, 0));
}

static void test_history_count_above_the_maximum_is_cut() {
    PortalHistory h = history_at_every_maximum();
    h.count = 200;  // a corrupt count never reads past the entries
    char buf[PORTAL_HISTORY_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_history(h, 1, buf, sizeof(buf)));
    const PJson j = portal_json_parse(buf);
    TEST_ASSERT_EQUAL(PORTAL_HISTORY_MAX, static_cast<int>(j.at("count").n));
    TEST_ASSERT_EQUAL(PORTAL_HISTORY_MAX, j.at("grinds").a.size());
}

static void test_history_result_names() {
    TEST_ASSERT_EQUAL_STRING("complete", portal_history_result_name(static_cast<uint8_t>(GrindHistoryResult::COMPLETE)));
    TEST_ASSERT_EQUAL_STRING("max_pulses", portal_history_result_name(static_cast<uint8_t>(GrindHistoryResult::MAX_PULSES)));
    TEST_ASSERT_EQUAL_STRING("overshoot", portal_history_result_name(static_cast<uint8_t>(GrindHistoryResult::OVERSHOOT)));
    TEST_ASSERT_EQUAL_STRING("timeout", portal_history_result_name(static_cast<uint8_t>(GrindHistoryResult::TIMEOUT)));
    TEST_ASSERT_EQUAL_STRING("unknown", portal_history_result_name(static_cast<uint8_t>(GrindHistoryResult::UNKNOWN)));
    for (unsigned code = 4; code < 255; code++) {
        TEST_ASSERT_EQUAL_STRING("unknown", portal_history_result_name(static_cast<uint8_t>(code)));
    }
}

static void test_history_negative_zero_and_tiny_errors_print_as_zero() {
    PortalHistory h;
    h.loaded = true;
    h.count = 2;
    h.entries[0].final_g = -0.0f;
    h.entries[1].final_g = -0.0004f;
    char buf[PORTAL_HISTORY_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_history(h, 0, buf, sizeof(buf)));
    TEST_ASSERT_NULL(strstr(buf, "-0.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"final_g\":0.0,"));
}

void run_portal_insights_tests() {
    RUN_TEST(test_history_snapshot_converts_field_for_field);
    RUN_TEST(test_history_snapshot_not_loaded_and_empty);
    RUN_TEST(test_history_snapshot_count_is_bounded);
    RUN_TEST(test_history_limits_agree);
    RUN_TEST(test_history_vectors);
    RUN_TEST(test_history_empty_is_the_documented_reply);
    RUN_TEST(test_history_worst_case_fits_its_buffer_with_margin);
    RUN_TEST(test_history_truncation_never_yields_a_complete_reply);
    RUN_TEST(test_history_count_above_the_maximum_is_cut);
    RUN_TEST(test_history_result_names);
    RUN_TEST(test_history_negative_zero_and_tiny_errors_print_as_zero);
}
