// Dose save policy (GLinkDoseSave, chg024 part B, trimmed in chg025): pending value with an injected clock.
#include <stdint.h>
#include <unity.h>
#include "GLinkDoseSave.h"

using namespace glinkc;

namespace {

constexpr uint32_t EXPIRY = DoseSaveLimits::PENDING_MS;

bool take_now(DoseSave& d, uint32_t now, int32_t* t = nullptr) {
    int32_t tt = 0;
    return d.take(true, now, t ? t : &tt);
}

void test_range_limits() {
    const int32_t bad[] = {49, 0, -10, 401, 600};  // 4.9, 40.1 g and worse
    for (int32_t v : bad) {
        DoseSave d;
        d.on_finished(v, 0);
        TEST_ASSERT_FALSE(d.has_pending());
        TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::OUT_OF_RANGE);
    }
    const int32_t good[] = {50, 400};  // 5.0 and 40.0 g
    for (int32_t v : good) {
        DoseSave d;
        d.on_finished(v, 0);
        int32_t t = 0;
        TEST_ASSERT_TRUE(take_now(d, 1, &t));
        TEST_ASSERT_EQUAL_INT32(v, t);
        TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::NONE);
    }
}

void test_pending_and_take_once() {
    DoseSave d;
    d.on_finished(181, 100);
    TEST_ASSERT_TRUE(d.has_pending());
    int32_t t = 0;
    TEST_ASSERT_TRUE(take_now(d, 200, &t));
    TEST_ASSERT_EQUAL_INT32(181, t);
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(take_now(d, 201));
    TEST_ASSERT_EQUAL_INT32(181, d.last_saved_tenths());
}

void test_newer_valid_replaces_pending() {
    DoseSave d;
    d.on_finished(181, 0);
    d.on_finished(190, 10);
    int32_t t = 0;
    TEST_ASSERT_TRUE(take_now(d, 11, &t));
    TEST_ASSERT_EQUAL_INT32(190, t);
    TEST_ASSERT_FALSE(take_now(d, 12));
}

void test_out_of_range_keeps_the_older_pending() {
    DoseSave d;
    d.on_finished(181, 0);
    d.on_finished(600, 10);
    TEST_ASSERT_TRUE(d.last_skip() == DoseSkip::OUT_OF_RANGE);
    int32_t t = 0;
    TEST_ASSERT_TRUE(take_now(d, 11, &t));
    TEST_ASSERT_EQUAL_INT32(181, t);
}

void test_pending_is_kept_while_the_machine_cannot_apply() {
    DoseSave d;
    d.on_finished(181, 1000);
    int32_t t = 0;
    TEST_ASSERT_FALSE(d.take(false, 1001, &t));
    TEST_ASSERT_FALSE(d.take(false, 1000 + EXPIRY - 1, &t));
    TEST_ASSERT_TRUE(d.has_pending());
    TEST_ASSERT_TRUE(d.take(true, 1000 + EXPIRY - 1, &t));
    TEST_ASSERT_EQUAL_INT32(181, t);
}

void test_expiry_at_exactly_300000_ms() {
    DoseSave d;
    d.on_finished(181, 0);
    int32_t t = 0;
    TEST_ASSERT_FALSE(d.take(true, EXPIRY, &t));
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_EQUAL_INT32(0, d.last_saved_tenths());
    TEST_ASSERT_EQUAL_UINT32(300000u, EXPIRY);
}

void test_expiry_while_the_machine_cannot_apply() {
    DoseSave d;
    d.on_finished(181, 0);
    int32_t t = 0;
    TEST_ASSERT_FALSE(d.take(false, EXPIRY, &t));
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(d.take(true, EXPIRY + 1, &t));
    TEST_ASSERT_EQUAL_INT32(0, d.last_saved_tenths());
}

void test_expiry_across_the_millis_wrap() {
    const uint32_t start = 0xFFFFFFFFu - 1000u;
    int32_t t = 0;
    DoseSave d;
    d.on_finished(181, start);
    TEST_ASSERT_FALSE(d.take(false, start + 2000u, &t));
    TEST_ASSERT_TRUE(d.has_pending());  // 2 s later, past the wrap: still alive
    TEST_ASSERT_TRUE(d.take(true, start + EXPIRY - 1, &t));
    DoseSave e;
    e.on_finished(181, start);
    TEST_ASSERT_FALSE(e.take(true, start + EXPIRY, &t));
    TEST_ASSERT_FALSE(e.has_pending());
}

void test_cancel_drops_pending() {
    DoseSave d;
    d.on_finished(181, 0);
    d.cancel();
    TEST_ASSERT_FALSE(d.has_pending());
    TEST_ASSERT_FALSE(take_now(d, 1));
}

void test_last_saved_survives_cancel_and_later_skip() {
    DoseSave d;
    d.on_finished(181, 0);
    TEST_ASSERT_TRUE(take_now(d, 1));
    d.cancel();
    d.on_finished(700, 2);  // skipped
    TEST_ASSERT_EQUAL_INT32(181, d.last_saved_tenths());
}

}  // namespace

void run_glink_plugin_dosesave_tests() {
    RUN_TEST(test_range_limits);
    RUN_TEST(test_pending_and_take_once);
    RUN_TEST(test_newer_valid_replaces_pending);
    RUN_TEST(test_out_of_range_keeps_the_older_pending);
    RUN_TEST(test_pending_is_kept_while_the_machine_cannot_apply);
    RUN_TEST(test_expiry_at_exactly_300000_ms);
    RUN_TEST(test_expiry_while_the_machine_cannot_apply);
    RUN_TEST(test_expiry_across_the_millis_wrap);
    RUN_TEST(test_cancel_drops_pending);
    RUN_TEST(test_last_saved_survives_cancel_and_later_skip);
}
