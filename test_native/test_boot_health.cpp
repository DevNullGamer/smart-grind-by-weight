#include <unity.h>
#include "system/boot_health_logic.h"

static const uint32_t kAfter = 30000;

static void test_confirm_not_pending_never() {
    // USB-flashed, factory or already confirmed image: no pending state, nothing to confirm
    TEST_ASSERT_FALSE(boot_health_should_confirm(false, true, kAfter, kAfter));
    TEST_ASSERT_FALSE(boot_health_should_confirm(false, true, 10 * kAfter, kAfter));
    TEST_ASSERT_FALSE(boot_health_should_confirm(false, false, 0, kAfter));
}

static void test_confirm_waits_for_delay() {
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, true, 0, kAfter));
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, true, kAfter - 1, kAfter));
    TEST_ASSERT_TRUE(boot_health_should_confirm(true, true, kAfter, kAfter));
    TEST_ASSERT_TRUE(boot_health_should_confirm(true, true, kAfter + 1, kAfter));
    TEST_ASSERT_TRUE(boot_health_should_confirm(true, true, 0xFFFFFFFFu, kAfter));
}

static void test_confirm_needs_setup_done() {
    // setup() still running (or hung): never confirm, however late it is
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, false, kAfter, kAfter));
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, false, 0xFFFFFFFFu, kAfter));
}

static void test_confirm_zero_delay() {
    TEST_ASSERT_TRUE(boot_health_should_confirm(true, true, 0, 0));
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, false, 0, 0));
}

static void test_confirm_after_millis_wrap_does_not_confirm_early() {
    // uptime restarts at 0 after 49 days; a wrapped value is just a small uptime
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, true, 5, kAfter));
}

static void test_crash_due_off_by_default() {
    TEST_ASSERT_FALSE(boot_health_crash_due(0, true, true, 100000, 8000));
}

static void test_crash_due_only_when_pending() {
    // A USB-flashed test image (not pending) must never crash-loop
    TEST_ASSERT_FALSE(boot_health_crash_due(1, false, true, 100000, 8000));
    TEST_ASSERT_FALSE(boot_health_crash_due(2, false, true, 100000, 8000));
}

static void test_crash_due_timing_and_setup() {
    TEST_ASSERT_FALSE(boot_health_crash_due(1, true, true, 7999, 8000));
    TEST_ASSERT_TRUE(boot_health_crash_due(1, true, true, 8000, 8000));
    TEST_ASSERT_TRUE(boot_health_crash_due(2, true, true, 8001, 8000));
    TEST_ASSERT_FALSE(boot_health_crash_due(1, true, false, 8000, 8000));
}

static void test_crash_happens_before_confirmation() {
    // The configured defaults: the crash (8 s) always comes before the confirmation (30 s)
    TEST_ASSERT_TRUE(boot_health_crash_due(1, true, true, 8000, 8000));
    TEST_ASSERT_FALSE(boot_health_should_confirm(true, true, 8000, kAfter));
}

void run_boot_health_tests() {
    RUN_TEST(test_confirm_not_pending_never);
    RUN_TEST(test_confirm_waits_for_delay);
    RUN_TEST(test_confirm_needs_setup_done);
    RUN_TEST(test_confirm_zero_delay);
    RUN_TEST(test_confirm_after_millis_wrap_does_not_confirm_early);
    RUN_TEST(test_crash_due_off_by_default);
    RUN_TEST(test_crash_due_only_when_pending);
    RUN_TEST(test_crash_due_timing_and_setup);
    RUN_TEST(test_crash_happens_before_confirmation);
}
