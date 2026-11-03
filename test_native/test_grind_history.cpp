// Recent-grinds history, RAM side: result mapping, entry rules, the ring (chg016 step 10)
#include <unity.h>
#include <cmath>
#include <limits>
#include <vector>
#include "logging/grind_history.h"

static GrindHistoryEntry entry_n(uint32_t n) {
    GrindHistoryEntry e;
    e.session_id = n;
    e.ended_s = n * 10;
    e.time_ms = 10000 + n;
    e.motor_ms = 9000 + n;
    e.target = 18.0f;
    e.final_g = 18.0f + 0.1f * static_cast<float>(n % 5);
    e.profile = static_cast<uint8_t>(n % 3);
    e.pulses = static_cast<uint8_t>(n % 4);
    e.result = 0;
    return e;
}

static std::vector<uint32_t> ids_of(const GrindHistoryRing& ring, size_t max = 100) {
    GrindHistoryEntry out[GRIND_HISTORY_RING_CAPACITY + 4];
    const size_t cap = max < GRIND_HISTORY_RING_CAPACITY + 4 ? max : GRIND_HISTORY_RING_CAPACITY + 4;
    const uint8_t n = ring.copy_newest_first(out, cap);
    std::vector<uint32_t> ids;
    for (uint8_t i = 0; i < n; i++) ids.push_back(out[i].session_id);
    return ids;
}

// ---- result mapping ----

static void test_result_strings_map_to_codes() {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(grind_history_result_from_string("COMPLETE")));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(grind_history_result_from_string("COMPLETE - MAX PULSES")));
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(grind_history_result_from_string("OVERSHOOT")));
    TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(grind_history_result_from_string("TIMEOUT")));
}

static void test_unknown_result_strings_map_to_unknown() {
    const char* odd[] = {"", "STOPPED_BY_USER", "complete", "Complete", "COMPLETE ", " COMPLETE", "COMPLETE - MAX",
                         "COMPLETE - MAX PULSES!", "ERROR", "TIMEOUT\n"};
    for (const char* s : odd) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, static_cast<uint8_t>(grind_history_result_from_string(s)), s);
    }
    TEST_ASSERT_EQUAL_UINT8(255, static_cast<uint8_t>(grind_history_result_from_string(nullptr)));
}

static void test_result_codes_are_pinned() {
    // These values are stored in the files: renumbering would change old history
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(GrindHistoryResult::COMPLETE));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(GrindHistoryResult::MAX_PULSES));
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(GrindHistoryResult::OVERSHOOT));
    TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(GrindHistoryResult::TIMEOUT));
    TEST_ASSERT_EQUAL_UINT8(255, static_cast<uint8_t>(GrindHistoryResult::UNKNOWN));
    for (int code = 0; code < 256; code++) {
        const bool expected = code <= 3 || code == 255;
        TEST_ASSERT_EQUAL(expected, grind_history_result_valid(static_cast<uint8_t>(code)));
    }
}

// ---- entry rules ----

static void test_entry_validity_and_sanitize() {
    GrindHistoryEntry e = entry_n(1);
    TEST_ASSERT_TRUE(grind_history_entry_valid(e));

    GrindHistoryEntry bad = e;
    bad.target = std::numeric_limits<float>::quiet_NaN();
    TEST_ASSERT_FALSE(grind_history_entry_valid(bad));
    bad = e;
    bad.final_g = std::numeric_limits<float>::infinity();
    TEST_ASSERT_FALSE(grind_history_entry_valid(bad));
    bad = e;
    bad.profile = 3;
    TEST_ASSERT_FALSE(grind_history_entry_valid(bad));
    bad = e;
    bad.mode = 2;
    TEST_ASSERT_FALSE(grind_history_entry_valid(bad));
    bad = e;
    bad.result = 4;
    TEST_ASSERT_FALSE(grind_history_entry_valid(bad));

    // sanitize makes every one of them storable and leaves a good entry alone
    GrindHistoryEntry s = e;
    grind_history_entry_sanitize(&s);
    TEST_ASSERT_EQUAL_UINT32(e.session_id, s.session_id);
    TEST_ASSERT_EQUAL_FLOAT(e.final_g, s.final_g);
    bad.target = -std::numeric_limits<float>::infinity();
    bad.final_g = std::numeric_limits<float>::quiet_NaN();
    bad.profile = 200;
    bad.mode = 9;
    bad.result = 77;
    grind_history_entry_sanitize(&bad);
    TEST_ASSERT_TRUE(grind_history_entry_valid(bad));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, bad.target);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, bad.final_g);
    TEST_ASSERT_EQUAL_UINT8(GRIND_HISTORY_PROFILE_MAX, bad.profile);
    TEST_ASSERT_EQUAL_UINT8(GRIND_HISTORY_MODE_MAX, bad.mode);
    TEST_ASSERT_EQUAL_UINT8(255, bad.result);
    grind_history_entry_sanitize(nullptr);  // no crash
}

static void test_negative_and_extreme_weights_are_valid() {
    GrindHistoryEntry e = entry_n(1);
    e.final_g = -3.5f;
    e.target = std::numeric_limits<float>::max();
    TEST_ASSERT_TRUE(grind_history_entry_valid(e));
    e.target = std::numeric_limits<float>::denorm_min();
    TEST_ASSERT_TRUE(grind_history_entry_valid(e));
}

// ---- ring ----

static void test_ring_empty() {
    GrindHistoryRing ring;
    TEST_ASSERT_EQUAL_UINT8(0, ring.count());
    TEST_ASSERT_EQUAL_UINT32(0, ring.sequence());
    GrindHistoryEntry out[3];
    TEST_ASSERT_EQUAL_UINT8(0, ring.copy_newest_first(out, 3));
    TEST_ASSERT_EQUAL_UINT8(0, ring.copy_newest_first(out, 0));
    TEST_ASSERT_EQUAL_UINT8(0, ring.copy_newest_first(nullptr, 3));
}

static void test_ring_one_entry_and_boundaries() {
    GrindHistoryRing ring;
    ring.push(entry_n(7));
    TEST_ASSERT_EQUAL_UINT8(1, ring.count());
    TEST_ASSERT_EQUAL_UINT32(1, ring.sequence());
    TEST_ASSERT_EQUAL(1, ids_of(ring).size());
    TEST_ASSERT_EQUAL_UINT32(7, ids_of(ring)[0]);

    // 9, 10, 11 entries: the boundary of the capacity
    for (uint32_t total : {9u, 10u, 11u}) {
        GrindHistoryRing r;
        for (uint32_t i = 1; i <= total; i++) r.push(entry_n(i));
        const std::vector<uint32_t> ids = ids_of(r);
        const size_t expected_count = total < GRIND_HISTORY_RING_CAPACITY ? total : GRIND_HISTORY_RING_CAPACITY;
        TEST_ASSERT_EQUAL(expected_count, ids.size());
        for (size_t i = 0; i < ids.size(); i++) TEST_ASSERT_EQUAL_UINT32(total - i, ids[i]);
        TEST_ASSERT_EQUAL_UINT32(total, r.sequence());
    }
}

static void test_ring_wraps_many_times_and_keeps_order() {
    GrindHistoryRing ring;
    for (uint32_t i = 1; i <= 1003; i++) ring.push(entry_n(i));
    const std::vector<uint32_t> ids = ids_of(ring);
    TEST_ASSERT_EQUAL(GRIND_HISTORY_RING_CAPACITY, ids.size());
    for (size_t i = 0; i < ids.size(); i++) TEST_ASSERT_EQUAL_UINT32(1003 - i, ids[i]);
    TEST_ASSERT_EQUAL_UINT32(1003, ring.sequence());
    TEST_ASSERT_EQUAL_UINT8(GRIND_HISTORY_RING_CAPACITY, ring.count());
}

static void test_ring_copy_respects_max() {
    GrindHistoryRing ring;
    for (uint32_t i = 1; i <= 25; i++) ring.push(entry_n(i));
    const std::vector<uint32_t> three = ids_of(ring, 3);
    TEST_ASSERT_EQUAL(3, three.size());
    TEST_ASSERT_EQUAL_UINT32(25, three[0]);
    TEST_ASSERT_EQUAL_UINT32(23, three[2]);
    // asking for more than the ring holds returns what it holds
    GrindHistoryEntry big[GRIND_HISTORY_RING_CAPACITY + 4];
    TEST_ASSERT_EQUAL_UINT8(GRIND_HISTORY_RING_CAPACITY, ring.copy_newest_first(big, GRIND_HISTORY_RING_CAPACITY + 4));
}

static void test_ring_sequence_wraps_harmlessly() {
    GrindHistoryRing ring(0xFFFFFFFEu);
    for (uint32_t i = 1; i <= 4; i++) ring.push(entry_n(i));
    TEST_ASSERT_EQUAL_UINT32(2, ring.sequence());  // 0xFFFFFFFE + 4 wraps to 2
    const std::vector<uint32_t> ids = ids_of(ring);
    TEST_ASSERT_EQUAL(4, ids.size());
    TEST_ASSERT_EQUAL_UINT32(4, ids[0]);
    TEST_ASSERT_EQUAL_UINT32(1, ids[3]);
    TEST_ASSERT_EQUAL_UINT8(4, ring.count());
}

static void test_ring_grind_id_wrap_is_just_data() {
    // The grind id wraps at 2^32; the ring orders by push order, never by id
    GrindHistoryRing ring;
    for (uint32_t id : {0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u}) ring.push(entry_n(id));
    const std::vector<uint32_t> ids = ids_of(ring);
    TEST_ASSERT_EQUAL_UINT32(1, ids[0]);
    TEST_ASSERT_EQUAL_UINT32(0, ids[1]);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, ids[2]);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEu, ids[3]);
}

static void test_ring_duplicate_ids_are_kept() {
    GrindHistoryRing ring;
    ring.push(entry_n(5));
    ring.push(entry_n(5));
    const std::vector<uint32_t> ids = ids_of(ring);
    TEST_ASSERT_EQUAL(2, ids.size());
}

static void test_ring_time_mode_entry_is_stored_as_is() {
    GrindHistoryRing ring;
    GrindHistoryEntry e = entry_n(3);
    e.mode = 1;
    e.target = 12.5f;  // seconds
    e.result = 3;
    ring.push(e);
    GrindHistoryEntry out[1];
    TEST_ASSERT_EQUAL_UINT8(1, ring.copy_newest_first(out, 1));
    TEST_ASSERT_EQUAL_UINT8(1, out[0].mode);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, out[0].target);
    TEST_ASSERT_EQUAL_UINT8(3, out[0].result);
}

static void test_ring_mark_earlier_boot_and_clear() {
    GrindHistoryRing ring;
    for (uint32_t i = 1; i <= 3; i++) ring.push(entry_n(i));
    GrindHistoryEntry out[3];
    ring.copy_newest_first(out, 3);
    TEST_ASSERT_FALSE(out[0].earlier_boot);
    ring.mark_earlier_boot();
    ring.copy_newest_first(out, 3);
    for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(out[i].earlier_boot);
    ring.push(entry_n(4));  // a grind after the marking is "this boot" again
    ring.copy_newest_first(out, 3);
    TEST_ASSERT_FALSE(out[0].earlier_boot);
    TEST_ASSERT_TRUE(out[1].earlier_boot);
    ring.clear();
    TEST_ASSERT_EQUAL_UINT8(0, ring.count());
    TEST_ASSERT_EQUAL_UINT32(4, ring.sequence());  // the push counter is not reset
}

void run_grind_history_tests() {
    RUN_TEST(test_result_strings_map_to_codes);
    RUN_TEST(test_unknown_result_strings_map_to_unknown);
    RUN_TEST(test_result_codes_are_pinned);
    RUN_TEST(test_entry_validity_and_sanitize);
    RUN_TEST(test_negative_and_extreme_weights_are_valid);
    RUN_TEST(test_ring_empty);
    RUN_TEST(test_ring_one_entry_and_boundaries);
    RUN_TEST(test_ring_wraps_many_times_and_keeps_order);
    RUN_TEST(test_ring_copy_respects_max);
    RUN_TEST(test_ring_sequence_wraps_harmlessly);
    RUN_TEST(test_ring_grind_id_wrap_is_just_data);
    RUN_TEST(test_ring_duplicate_ids_are_kept);
    RUN_TEST(test_ring_time_mode_entry_is_stored_as_is);
    RUN_TEST(test_ring_mark_earlier_boot_and_clear);
}
