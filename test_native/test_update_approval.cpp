#include <unity.h>
#include "network/update_approval.h"

static const uint32_t kTimeout = 30000;
static const uint32_t kBackoff = 30000;

static void test_request_then_accept() {
    UpdateApproval a;
    TEST_ASSERT_EQUAL(ApprovalRequestResult::OK, approval_request(&a, 1000, kTimeout, kBackoff));
    TEST_ASSERT_EQUAL(ApprovalState::PENDING, approval_poll(&a, 1001));
    TEST_ASSERT_TRUE(approval_resolve(&a, true, 2000));
    TEST_ASSERT_EQUAL(ApprovalState::ACCEPTED, approval_poll(&a, 2001));
    // No back-off after an accept; clear returns to IDLE and a new request works
    approval_clear(&a);
    TEST_ASSERT_EQUAL(ApprovalState::IDLE, a.state);
    TEST_ASSERT_EQUAL(ApprovalRequestResult::OK, approval_request(&a, 2002, kTimeout, kBackoff));
}

static void test_double_request_is_refused() {
    UpdateApproval a;
    TEST_ASSERT_EQUAL(ApprovalRequestResult::OK, approval_request(&a, 0, kTimeout, kBackoff));
    TEST_ASSERT_EQUAL(ApprovalRequestResult::ALREADY_PENDING, approval_request(&a, 10, kTimeout, kBackoff));
    TEST_ASSERT_EQUAL(ApprovalState::PENDING, a.state);
}

static void test_decline_starts_backoff() {
    UpdateApproval a;
    approval_request(&a, 1000, kTimeout, kBackoff);
    TEST_ASSERT_TRUE(approval_resolve(&a, false, 5000));
    TEST_ASSERT_EQUAL(ApprovalState::DECLINED, approval_poll(&a, 5000));
    approval_clear(&a);
    TEST_ASSERT_EQUAL_UINT32(kBackoff, approval_backoff_remaining_ms(&a, 5000));
    TEST_ASSERT_EQUAL_UINT32(1, approval_backoff_remaining_ms(&a, 5000 + kBackoff - 1));
    TEST_ASSERT_EQUAL(ApprovalRequestResult::BACKOFF, approval_request(&a, 5000 + kBackoff - 1, kTimeout, kBackoff));
    TEST_ASSERT_EQUAL(ApprovalState::IDLE, a.state);
    TEST_ASSERT_EQUAL_UINT32(0, approval_backoff_remaining_ms(&a, 5000 + kBackoff));
    TEST_ASSERT_EQUAL(ApprovalRequestResult::OK, approval_request(&a, 5000 + kBackoff, kTimeout, kBackoff));
}

static void test_expiry() {
    UpdateApproval a;
    approval_request(&a, 1000, kTimeout, kBackoff);
    TEST_ASSERT_EQUAL_UINT32(kTimeout, approval_remaining_ms(&a, 1000));
    TEST_ASSERT_EQUAL(ApprovalState::PENDING, approval_poll(&a, 1000 + kTimeout - 1));
    TEST_ASSERT_EQUAL(ApprovalState::EXPIRED, approval_poll(&a, 1000 + kTimeout));
    TEST_ASSERT_EQUAL_UINT32(0, approval_remaining_ms(&a, 1000 + kTimeout));
    // An expiry is not a decline: no back-off
    approval_clear(&a);
    TEST_ASSERT_EQUAL_UINT32(0, approval_backoff_remaining_ms(&a, 1000 + kTimeout));
}

static void test_late_answer_does_not_overwrite_expiry() {
    UpdateApproval a;
    approval_request(&a, 0, kTimeout, kBackoff);
    // Nobody polled yet, but the time is up: the accept arrives too late
    TEST_ASSERT_FALSE(approval_resolve(&a, true, kTimeout + 5));
    TEST_ASSERT_EQUAL(ApprovalState::EXPIRED, a.state);
}

static void test_cancel_and_busy() {
    UpdateApproval a;
    approval_request(&a, 0, kTimeout, kBackoff);
    TEST_ASSERT_TRUE(approval_cancel(&a));
    TEST_ASSERT_EQUAL(ApprovalState::CANCELLED, a.state);
    TEST_ASSERT_FALSE(approval_cancel(&a));
    TEST_ASSERT_FALSE(approval_resolve(&a, true, 10));  // a late tap after the cancel changes nothing
    TEST_ASSERT_EQUAL(ApprovalState::CANCELLED, a.state);
    approval_clear(&a);

    approval_request(&a, 0, kTimeout, kBackoff);
    TEST_ASSERT_TRUE(approval_resolve_busy(&a));
    TEST_ASSERT_EQUAL(ApprovalState::UI_BUSY, a.state);
    TEST_ASSERT_FALSE(approval_resolve_busy(&a));
    // Neither starts a back-off
    approval_clear(&a);
    TEST_ASSERT_EQUAL_UINT32(0, approval_backoff_remaining_ms(&a, 1));
}

static void test_nothing_to_resolve_when_idle() {
    UpdateApproval a;
    TEST_ASSERT_FALSE(approval_resolve(&a, true, 0));
    TEST_ASSERT_FALSE(approval_resolve_busy(&a));
    TEST_ASSERT_FALSE(approval_cancel(&a));
    TEST_ASSERT_EQUAL(ApprovalState::IDLE, approval_poll(&a, 123));
    TEST_ASSERT_EQUAL_UINT32(0, approval_remaining_ms(&a, 123));
}

static void test_stale_finished_state_does_not_block_a_new_request() {
    UpdateApproval a;
    approval_request(&a, 0, kTimeout, kBackoff);
    approval_resolve(&a, true, 1);  // ACCEPTED and never cleared
    TEST_ASSERT_EQUAL(ApprovalRequestResult::OK, approval_request(&a, 2, kTimeout, kBackoff));
    TEST_ASSERT_EQUAL(ApprovalState::PENDING, a.state);
}

static void test_time_wraps() {
    UpdateApproval a;
    const uint32_t start = 0xFFFFFFFFu - 1000;  // the timeout spans the millis() wrap
    approval_request(&a, start, kTimeout, kBackoff);
    TEST_ASSERT_EQUAL(ApprovalState::PENDING, approval_poll(&a, start + 20000));
    TEST_ASSERT_EQUAL_UINT32(kTimeout - 20000, approval_remaining_ms(&a, start + 20000));
    TEST_ASSERT_EQUAL(ApprovalState::EXPIRED, approval_poll(&a, start + kTimeout));

    approval_clear(&a);
    approval_request(&a, start, kTimeout, kBackoff);
    TEST_ASSERT_TRUE(approval_resolve(&a, false, start + 100));
    approval_clear(&a);
    TEST_ASSERT_EQUAL_UINT32(kBackoff - 5000, approval_backoff_remaining_ms(&a, start + 100 + 5000));
    TEST_ASSERT_EQUAL_UINT32(0, approval_backoff_remaining_ms(&a, start + 100 + kBackoff));
}

static void test_old_backoff_does_not_rearm_after_clock_wrap() {
    UpdateApproval a;
    approval_request(&a, 1000, kTimeout, kBackoff);
    approval_resolve(&a, false, 1000);
    approval_clear(&a);
    // Not consulted for ~25 days: the deadline now looks far ahead and must not count
    const uint32_t later = 1000 + kBackoff + 0x80000000u;
    TEST_ASSERT_EQUAL_UINT32(0, approval_backoff_remaining_ms(&a, later));
    TEST_ASSERT_EQUAL(ApprovalRequestResult::OK, approval_request(&a, later, kTimeout, kBackoff));
}

void run_update_approval_tests() {
    RUN_TEST(test_request_then_accept);
    RUN_TEST(test_double_request_is_refused);
    RUN_TEST(test_decline_starts_backoff);
    RUN_TEST(test_expiry);
    RUN_TEST(test_late_answer_does_not_overwrite_expiry);
    RUN_TEST(test_cancel_and_busy);
    RUN_TEST(test_nothing_to_resolve_when_idle);
    RUN_TEST(test_stale_finished_state_does_not_block_a_new_request);
    RUN_TEST(test_time_wraps);
    RUN_TEST(test_old_backoff_does_not_rearm_after_clock_wrap);
}
