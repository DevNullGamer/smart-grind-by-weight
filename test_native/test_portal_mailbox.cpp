#include <unity.h>
#include <string.h>
#include "config/portal.h"
#include "network/portal_mailbox.h"

namespace {

const uint32_t kPickup = PORTAL_PICKUP_TIMEOUT_MS;
const uint32_t kRun = PORTAL_RUN_TIMEOUT_MS;

PortalPatch patch_with_brightness(float percent) {
    PortalPatch p;
    p.present = 1u << PF_BRIGHTNESS_NORMAL;
    p.v[PF_BRIGHTNESS_NORMAL] = percent;
    return p;
}

PortalMailboxResult ok_result(float brightness) {
    PortalMailboxResult r;
    r.applied = portal_default_values();
    r.applied.v[PF_BRIGHTNESS_NORMAL] = brightness;
    r.adjusted_mask = 1u << PF_BRIGHTNESS_NORMAL;
    return r;
}

}  // namespace

static void test_happy_path_returns_the_answer_and_goes_idle() {
    PortalMailbox box;
    TEST_ASSERT_EQUAL(PortalError::NONE, portal_mailbox_submit(&box, patch_with_brightness(80), 1000));
    TEST_ASSERT_EQUAL(PortalMailboxState::QUEUED, box.state);
    PortalMailboxResult out;
    TEST_ASSERT_EQUAL(PortalMailboxWait::WAITING, portal_mailbox_poll(&box, 1001, kPickup, kRun, &out));

    PortalPatch taken;
    TEST_ASSERT_TRUE(portal_mailbox_take(&box, &taken, 1005));
    TEST_ASSERT_TRUE(taken.has(PF_BRIGHTNESS_NORMAL));
    TEST_ASSERT_EQUAL_FLOAT(80.0f, taken.v[PF_BRIGHTNESS_NORMAL]);
    TEST_ASSERT_EQUAL(PortalMailboxState::TAKEN, box.state);
    TEST_ASSERT_EQUAL(PortalMailboxWait::WAITING, portal_mailbox_poll(&box, 1006, kPickup, kRun, &out));

    portal_mailbox_complete(&box, ok_result(80));
    TEST_ASSERT_EQUAL(PortalMailboxWait::DONE, portal_mailbox_poll(&box, 1007, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalError::NONE, out.error);
    TEST_ASSERT_EQUAL_FLOAT(80.0f, out.applied.v[PF_BRIGHTNESS_NORMAL]);
    TEST_ASSERT_EQUAL_UINT32(1u << PF_BRIGHTNESS_NORMAL, out.adjusted_mask);
    TEST_ASSERT_EQUAL(PortalMailboxState::IDLE, box.state);
    TEST_ASSERT_FALSE(portal_mailbox_busy(box));
}

static void test_a_refusal_travels_in_the_result() {
    PortalMailbox box;
    portal_mailbox_submit(&box, patch_with_brightness(80), 0);
    portal_mailbox_take(&box, nullptr, 1);
    PortalMailboxResult refusal;
    refusal.error = PortalError::NOT_READY;
    portal_mailbox_complete(&box, refusal);
    PortalMailboxResult out;
    TEST_ASSERT_EQUAL(PortalMailboxWait::DONE, portal_mailbox_poll(&box, 2, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalError::NOT_READY, out.error);
}

static void test_one_request_in_flight_at_a_time() {
    PortalMailbox box;
    TEST_ASSERT_EQUAL(PortalError::NONE, portal_mailbox_submit(&box, patch_with_brightness(10), 0));
    // queued, taken and done-but-not-collected all count as in flight
    TEST_ASSERT_EQUAL(PortalError::COMMAND_IN_FLIGHT, portal_mailbox_submit(&box, patch_with_brightness(20), 1));
    portal_mailbox_take(&box, nullptr, 2);
    TEST_ASSERT_EQUAL(PortalError::COMMAND_IN_FLIGHT, portal_mailbox_submit(&box, patch_with_brightness(20), 3));
    portal_mailbox_complete(&box, ok_result(10));
    TEST_ASSERT_EQUAL(PortalError::COMMAND_IN_FLIGHT, portal_mailbox_submit(&box, patch_with_brightness(20), 4));
    // the refused submit must not have replaced the queued request
    PortalMailboxResult out;
    portal_mailbox_poll(&box, 5, kPickup, kRun, &out);
    TEST_ASSERT_EQUAL(PortalError::NONE, portal_mailbox_submit(&box, patch_with_brightness(30), 6));
    PortalPatch taken;
    TEST_ASSERT_TRUE(portal_mailbox_take(&box, &taken, 7));
    TEST_ASSERT_EQUAL_FLOAT(30.0f, taken.v[PF_BRIGHTNESS_NORMAL]);
}

static void test_a_request_nobody_takes_is_withdrawn() {
    PortalMailbox box;
    portal_mailbox_submit(&box, patch_with_brightness(80), 1000);
    PortalMailboxResult out;
    TEST_ASSERT_EQUAL(PortalMailboxWait::WAITING, portal_mailbox_poll(&box, 1000 + kPickup - 1, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalMailboxWait::TIMEOUT, portal_mailbox_poll(&box, 1000 + kPickup, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalMailboxState::IDLE, box.state);
    // withdrawn means the UI task can no longer take it, and a new request is accepted
    TEST_ASSERT_FALSE(portal_mailbox_take(&box, nullptr, 2000));
    TEST_ASSERT_EQUAL(PortalError::NONE, portal_mailbox_submit(&box, patch_with_brightness(40), 2001));
}

static void test_a_taken_request_is_abandoned_not_withdrawn() {
    PortalMailbox box;
    portal_mailbox_submit(&box, patch_with_brightness(80), 1000);
    TEST_ASSERT_TRUE(portal_mailbox_take(&box, nullptr, 1400));
    PortalMailboxResult out;
    // the pickup timeout no longer applies once it is taken; the run timeout counts from the take
    TEST_ASSERT_EQUAL(PortalMailboxWait::WAITING, portal_mailbox_poll(&box, 1000 + kPickup + 100, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalMailboxWait::TIMEOUT, portal_mailbox_poll(&box, 1400 + kRun, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalMailboxState::TAKEN, box.state);
    TEST_ASSERT_TRUE(box.abandoned);
    // still busy: nothing new is admitted while the UI task may still be writing
    TEST_ASSERT_EQUAL(PortalError::COMMAND_IN_FLIGHT, portal_mailbox_submit(&box, patch_with_brightness(5), 4000));
    // the UI task finishes late: the answer is dropped and the mailbox is free again
    portal_mailbox_complete(&box, ok_result(80));
    TEST_ASSERT_EQUAL(PortalMailboxState::IDLE, box.state);
    TEST_ASSERT_FALSE(box.abandoned);
    TEST_ASSERT_EQUAL(PortalError::NONE, portal_mailbox_submit(&box, patch_with_brightness(5), 4001));
    // and the late answer must not leak into the next request
    TEST_ASSERT_TRUE(portal_mailbox_take(&box, nullptr, 4002));
    portal_mailbox_complete(&box, ok_result(5));
    TEST_ASSERT_EQUAL(PortalMailboxWait::DONE, portal_mailbox_poll(&box, 4003, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, out.applied.v[PF_BRIGHTNESS_NORMAL]);
}

static void test_state_misuse_is_ignored() {
    PortalMailbox box;
    PortalMailboxResult out;
    PortalPatch taken;
    TEST_ASSERT_FALSE(portal_mailbox_take(&box, &taken, 0));              // nothing queued
    portal_mailbox_complete(&box, ok_result(1));                          // nothing taken
    TEST_ASSERT_EQUAL(PortalMailboxState::IDLE, box.state);
    TEST_ASSERT_EQUAL(PortalMailboxWait::TIMEOUT, portal_mailbox_poll(&box, 0, kPickup, kRun, &out));  // no request

    portal_mailbox_submit(&box, patch_with_brightness(80), 0);
    portal_mailbox_complete(&box, ok_result(80));                         // queued, not taken: cannot complete
    TEST_ASSERT_EQUAL(PortalMailboxState::QUEUED, box.state);
    TEST_ASSERT_TRUE(portal_mailbox_take(&box, nullptr, 1));
    TEST_ASSERT_FALSE(portal_mailbox_take(&box, nullptr, 2));             // cannot be taken twice
}

static void test_timeouts_survive_the_millis_wrap() {
    PortalMailbox box;
    const uint32_t start = 0xFFFFFFFFu - 100;
    portal_mailbox_submit(&box, patch_with_brightness(80), start);
    PortalMailboxResult out;
    TEST_ASSERT_EQUAL(PortalMailboxWait::WAITING, portal_mailbox_poll(&box, start + 200, kPickup, kRun, &out));  // wrapped to 99
    TEST_ASSERT_EQUAL(PortalMailboxWait::TIMEOUT, portal_mailbox_poll(&box, start + kPickup, kPickup, kRun, &out));

    portal_mailbox_submit(&box, patch_with_brightness(80), start);
    portal_mailbox_take(&box, nullptr, start);
    TEST_ASSERT_EQUAL(PortalMailboxWait::WAITING, portal_mailbox_poll(&box, start + kRun - 1, kPickup, kRun, &out));
    TEST_ASSERT_EQUAL(PortalMailboxWait::TIMEOUT, portal_mailbox_poll(&box, start + kRun, kPickup, kRun, &out));
}

static void test_the_patch_and_result_are_copies() {
    PortalMailbox box;
    PortalPatch original = patch_with_brightness(80);
    portal_mailbox_submit(&box, original, 0);
    original.v[PF_BRIGHTNESS_NORMAL] = 5;  // the caller's struct changes after the hand-over
    PortalPatch taken;
    portal_mailbox_take(&box, &taken, 1);
    TEST_ASSERT_EQUAL_FLOAT(80.0f, taken.v[PF_BRIGHTNESS_NORMAL]);

    PortalMailboxResult result = ok_result(80);
    result.device_name_changed = true;
    strcpy(result.applied.device_name, "espresso-1");
    portal_mailbox_complete(&box, result);
    result.applied.v[PF_BRIGHTNESS_NORMAL] = 1;
    PortalMailboxResult out;
    portal_mailbox_poll(&box, 2, kPickup, kRun, &out);
    TEST_ASSERT_EQUAL_FLOAT(80.0f, out.applied.v[PF_BRIGHTNESS_NORMAL]);
    TEST_ASSERT_TRUE(out.device_name_changed);
    TEST_ASSERT_EQUAL_STRING("espresso-1", out.applied.device_name);
}

static void test_snapshot_freshness() {
    TEST_ASSERT_FALSE(portal_snapshot_fresh(false, 1000, 1000, PORTAL_SNAPSHOT_STALE_MS));  // never published
    TEST_ASSERT_TRUE(portal_snapshot_fresh(true, 1000, 1000, PORTAL_SNAPSHOT_STALE_MS));
    TEST_ASSERT_TRUE(portal_snapshot_fresh(true, 1000, 1000 + PORTAL_SNAPSHOT_STALE_MS - 1, PORTAL_SNAPSHOT_STALE_MS));
    TEST_ASSERT_FALSE(portal_snapshot_fresh(true, 1000, 1000 + PORTAL_SNAPSHOT_STALE_MS, PORTAL_SNAPSHOT_STALE_MS));
    // across the millis() wrap
    TEST_ASSERT_TRUE(portal_snapshot_fresh(true, 0xFFFFFFF0u, 0x10u, PORTAL_SNAPSHOT_STALE_MS));
    TEST_ASSERT_FALSE(portal_snapshot_fresh(true, 0xFFFFFFF0u, 0xFFFFFFF0u + PORTAL_SNAPSHOT_STALE_MS, PORTAL_SNAPSHOT_STALE_MS));
}

void run_portal_mailbox_tests() {
    RUN_TEST(test_happy_path_returns_the_answer_and_goes_idle);
    RUN_TEST(test_a_refusal_travels_in_the_result);
    RUN_TEST(test_one_request_in_flight_at_a_time);
    RUN_TEST(test_a_request_nobody_takes_is_withdrawn);
    RUN_TEST(test_a_taken_request_is_abandoned_not_withdrawn);
    RUN_TEST(test_state_misuse_is_ignored);
    RUN_TEST(test_timeouts_survive_the_millis_wrap);
    RUN_TEST(test_the_patch_and_result_are_copies);
    RUN_TEST(test_snapshot_freshness);
}
