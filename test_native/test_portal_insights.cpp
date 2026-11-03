// Web panel view of the history (chg016 step 11): the copy the UI task publishes
#include <unity.h>
#include "logging/grind_history.h"
#include "network/portal_insights.h"

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

void run_portal_insights_tests() {
    RUN_TEST(test_history_snapshot_converts_field_for_field);
    RUN_TEST(test_history_snapshot_not_loaded_and_empty);
    RUN_TEST(test_history_snapshot_count_is_bounded);
    RUN_TEST(test_history_limits_agree);
}
