#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "network/portal_apply.h"

namespace {

// Records every write in order; can fail the Nth write (counting field and name writes together)
class FakeTarget : public PortalApplyTarget {
public:
    struct Call {
        bool is_name;
        size_t index;
        float value;
        std::string name;
    };
    std::vector<Call> calls;
    int fail_at = -1;                 // zero-based call number that fails, -1 = never
    std::string stored_name = "grindbyweight";

    bool write_field(size_t index, float value) override {
        const bool ok = static_cast<int>(calls.size()) != fail_at;
        calls.push_back({false, index, value, ""});
        return ok;
    }
    bool write_device_name(const char* name, bool* changed) override {
        const bool ok = static_cast<int>(calls.size()) != fail_at;
        calls.push_back({true, PF_DEVICE_NAME, 0.0f, name});
        if (!ok) return false;
        *changed = stored_name != name;
        stored_name = name;
        return true;
    }
};

PortalPatch parse(const char* json) {
    PortalPatch patch;
    const PortalError err = portal_parse_patch(json, strlen(json), &patch);
    TEST_ASSERT_EQUAL_MESSAGE(PortalError::NONE, err, json);
    return patch;
}

}  // namespace

static void test_fields_are_written_in_index_order_and_the_name_last() {
    // Keys deliberately out of order in the body: the order of the writes does not follow the body
    const PortalPatch patch = parse(
        "{\"device_name\":\"espresso-1\",\"custom_time_s\":11,\"grind_mode\":\"time\",\"single_weight_g\":10,"
        "\"swipe\":true,\"brightness_normal\":70}");
    FakeTarget target;
    const PortalApplyOutcome outcome = portal_apply_patch(patch, portal_default_values(), target);
    TEST_ASSERT_EQUAL(PortalError::NONE, outcome.error);
    TEST_ASSERT_EQUAL_INT(-1, outcome.failed_field);
    TEST_ASSERT_EQUAL_UINT32(6, target.calls.size());
    const size_t expected[] = {PF_SWIPE, PF_BRIGHTNESS_NORMAL, PF_GRIND_MODE, PF_SINGLE_WEIGHT_G, PF_CUSTOM_TIME_S,
                               PF_DEVICE_NAME};
    for (size_t i = 0; i < 6; i++) TEST_ASSERT_EQUAL_UINT32(expected[i], target.calls[i].index);
    TEST_ASSERT_TRUE(target.calls[5].is_name);
    TEST_ASSERT_EQUAL_STRING("espresso-1", target.calls[5].name.c_str());
    TEST_ASSERT_TRUE(outcome.device_name_changed);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, target.calls[0].value);   // swipe true
    TEST_ASSERT_EQUAL_FLOAT(70.0f, target.calls[1].value);  // brightness percent
    TEST_ASSERT_EQUAL_FLOAT(1.0f, target.calls[2].value);   // grind mode time = 1
    uint32_t mask = 0;
    for (size_t i : expected) mask |= 1u << i;
    TEST_ASSERT_EQUAL_UINT32(mask, outcome.written_mask);
}

static void test_unchanged_fields_are_not_written() {
    PortalValues current = portal_default_values();
    const PortalPatch patch = parse("{\"brightness_normal\":100,\"logging\":true,\"double_weight_g\":18,\"purge_mode\":\"remove\"}");
    FakeTarget target;
    const PortalApplyOutcome outcome = portal_apply_patch(patch, current, target);
    // brightness 100, double 18 g and purge "remove" equal the defaults: only logging changes
    TEST_ASSERT_EQUAL_UINT32(1, target.calls.size());
    TEST_ASSERT_EQUAL_UINT32(PF_LOGGING, target.calls[0].index);
    TEST_ASSERT_EQUAL_UINT32(1u << PF_LOGGING, outcome.written_mask);
}

static void test_a_patch_equal_to_the_state_writes_nothing() {
    const PortalValues current = portal_default_values();
    const PortalPatch patch = parse("{\"ble_startup\":true,\"grind_mode\":\"weight\",\"single_time_s\":5}");
    FakeTarget target;
    const PortalApplyOutcome outcome = portal_apply_patch(patch, current, target);
    TEST_ASSERT_EQUAL_UINT32(0, target.calls.size());
    TEST_ASSERT_EQUAL(PortalError::NONE, outcome.error);
    TEST_ASSERT_EQUAL_UINT32(0, outcome.written_mask);
}

static void test_the_name_the_grinder_already_has_changes_nothing() {
    // The target decides (it knows the full stored name): the outcome reports it as not changed
    const PortalPatch patch = parse("{\"device_name\":\"grindbyweight\"}");
    FakeTarget target;
    const PortalApplyOutcome outcome = portal_apply_patch(patch, portal_default_values(), target);
    TEST_ASSERT_EQUAL(PortalError::NONE, outcome.error);
    TEST_ASSERT_FALSE(outcome.device_name_changed);
    TEST_ASSERT_EQUAL_UINT32(0, outcome.written_mask);
    TEST_ASSERT_EQUAL_UINT32(1, target.calls.size());  // it was offered to the target
}

static void test_the_name_is_offered_even_when_the_reported_name_matches() {
    // A stored name longer than 24 characters is reported shortened; sending the shortened one must still reach
    // the target, which compares against the full stored name
    PortalValues current = portal_default_values();
    strcpy(current.device_name, "a-really-long-name-of-24");
    const PortalPatch patch = parse("{\"device_name\":\"a-really-long-name-of-24\"}");
    FakeTarget target;
    target.stored_name = "a-really-long-name-of-24-and-more-characters";
    const PortalApplyOutcome outcome = portal_apply_patch(patch, current, target);
    TEST_ASSERT_TRUE(outcome.device_name_changed);
    TEST_ASSERT_EQUAL_STRING("a-really-long-name-of-24", target.stored_name.c_str());
}

static void test_a_failed_write_stops_everything_after_it() {
    // Calls: swipe(0) brightness(1) grind mode(2) single weight(3) ... name; fail the third (grind mode)
    const PortalPatch patch = parse(
        "{\"swipe\":true,\"brightness_normal\":70,\"grind_mode\":\"time\",\"single_weight_g\":10,\"device_name\":\"espresso-1\"}");
    FakeTarget target;
    target.fail_at = 2;
    const PortalApplyOutcome outcome = portal_apply_patch(patch, portal_default_values(), target);
    TEST_ASSERT_EQUAL(PortalError::STORE_FAILED, outcome.error);
    TEST_ASSERT_EQUAL_INT(PF_GRIND_MODE, outcome.failed_field);
    TEST_ASSERT_EQUAL_UINT32(3, target.calls.size());  // nothing after the failed write was attempted
    // the two before it stay applied and are reported so the screen follows them
    TEST_ASSERT_EQUAL_UINT32((1u << PF_SWIPE) | (1u << PF_BRIGHTNESS_NORMAL), outcome.written_mask);
    TEST_ASSERT_FALSE(outcome.device_name_changed);
    TEST_ASSERT_EQUAL_STRING("grindbyweight", target.stored_name.c_str());  // the name never changed
}

static void test_the_name_only_changes_when_everything_else_was_stored() {
    const PortalPatch patch = parse("{\"swipe\":true,\"device_name\":\"espresso-1\"}");
    FakeTarget target;
    target.fail_at = 0;  // the first field fails
    PortalApplyOutcome outcome = portal_apply_patch(patch, portal_default_values(), target);
    TEST_ASSERT_EQUAL(PortalError::STORE_FAILED, outcome.error);
    TEST_ASSERT_EQUAL_UINT32(1, target.calls.size());
    TEST_ASSERT_EQUAL_STRING("grindbyweight", target.stored_name.c_str());
}

static void test_a_failing_name_write_keeps_the_fields_before_it() {
    const PortalPatch patch = parse("{\"swipe\":true,\"device_name\":\"espresso-1\"}");
    FakeTarget target;
    target.fail_at = 1;  // the name write fails
    const PortalApplyOutcome outcome = portal_apply_patch(patch, portal_default_values(), target);
    TEST_ASSERT_EQUAL(PortalError::STORE_FAILED, outcome.error);
    TEST_ASSERT_EQUAL_INT(PF_DEVICE_NAME, outcome.failed_field);
    TEST_ASSERT_EQUAL_UINT32(1u << PF_SWIPE, outcome.written_mask);
    TEST_ASSERT_FALSE(outcome.device_name_changed);
}

static void test_every_profile_field_reaches_the_target_with_its_value() {
    const PortalPatch patch = parse(
        "{\"single_weight_g\":10.5,\"single_time_s\":4.5,\"double_weight_g\":19,\"double_time_s\":9,"
        "\"custom_weight_g\":22,\"custom_time_s\":13}");
    FakeTarget target;
    portal_apply_patch(patch, portal_default_values(), target);
    TEST_ASSERT_EQUAL_UINT32(6, target.calls.size());
    const float values[] = {10.5f, 4.5f, 19.0f, 9.0f, 22.0f, 13.0f};
    for (size_t i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_UINT32(PF_SINGLE_WEIGHT_G + i, target.calls[i].index);
        TEST_ASSERT_EQUAL_FLOAT(values[i], target.calls[i].value);
    }
}

static void test_the_address_flag_travels_from_outcome_to_reply() {
    PortalValues applied = portal_default_values();
    strcpy(applied.device_name, "espresso-1");
    PortalApplyOutcome outcome;
    outcome.device_name_changed = true;
    PortalMailboxResult result = portal_result_from_outcome(outcome, 1u << PF_BRIGHTNESS_NORMAL, applied);
    TEST_ASSERT_TRUE(result.device_name_changed);
    TEST_ASSERT_EQUAL(PortalError::NONE, result.error);
    TEST_ASSERT_EQUAL_UINT32(1u << PF_BRIGHTNESS_NORMAL, result.adjusted_mask);
    TEST_ASSERT_EQUAL_STRING("espresso-1", result.applied.device_name);
    char json[PORTAL_VALUES_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_patch_result(result, json, sizeof(json)));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"address\":\"espresso-1.local\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"adjusted\":[\"brightness_normal\"]"));

    // not changed: no address
    outcome.device_name_changed = false;
    result = portal_result_from_outcome(outcome, 0, applied);
    TEST_ASSERT_FALSE(result.device_name_changed);
    TEST_ASSERT_TRUE(portal_encode_patch_result(result, json, sizeof(json)));
    TEST_ASSERT_NULL(strstr(json, "address"));

    // end to end with the real apply: a changed name sets the flag, the same name does not
    FakeTarget target;
    outcome = portal_apply_patch(parse("{\"device_name\":\"espresso-1\"}"), portal_default_values(), target);
    TEST_ASSERT_TRUE(portal_result_from_outcome(outcome, 0, applied).device_name_changed);
    outcome = portal_apply_patch(parse("{\"device_name\":\"espresso-1\"}"), portal_default_values(), target);
    TEST_ASSERT_FALSE(portal_result_from_outcome(outcome, 0, applied).device_name_changed);
}

static void test_a_failed_write_result_carries_the_error() {
    FakeTarget target;
    target.fail_at = 0;
    const PortalApplyOutcome outcome = portal_apply_patch(parse("{\"swipe\":true}"), portal_default_values(), target);
    const PortalMailboxResult result = portal_result_from_outcome(outcome, 0, portal_default_values());
    TEST_ASSERT_EQUAL(PortalError::STORE_FAILED, result.error);
    TEST_ASSERT_FALSE(result.device_name_changed);
}

static void test_screen_timeouts_reach_the_target_one_key_at_a_time() {
    // Both keys: written in table order (dim, then off), both bits in the mask
    FakeTarget both;
    PortalApplyOutcome outcome =
        portal_apply_patch(parse("{\"screen_off_min\":240,\"screen_dim_min\":10}"), portal_default_values(), both);
    TEST_ASSERT_EQUAL(PortalError::NONE, outcome.error);
    TEST_ASSERT_EQUAL_UINT32(2, both.calls.size());
    TEST_ASSERT_EQUAL_UINT32(PF_SCREEN_DIM_MIN, both.calls[0].index);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, both.calls[0].value);
    TEST_ASSERT_EQUAL_UINT32(PF_SCREEN_OFF_MIN, both.calls[1].index);
    TEST_ASSERT_EQUAL_FLOAT(240.0f, both.calls[1].value);
    TEST_ASSERT_EQUAL_UINT32((1u << PF_SCREEN_DIM_MIN) | (1u << PF_SCREEN_OFF_MIN), outcome.written_mask);

    // Only one key: the other is never written (and so never reloaded)
    FakeTarget one;
    outcome = portal_apply_patch(parse("{\"screen_off_min\":0}"), portal_default_values(), one);
    TEST_ASSERT_EQUAL_UINT32(1, one.calls.size());
    TEST_ASSERT_EQUAL_UINT32(PF_SCREEN_OFF_MIN, one.calls[0].index);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, one.calls[0].value);  // Never is written as 0
    TEST_ASSERT_EQUAL_UINT32(1u << PF_SCREEN_OFF_MIN, outcome.written_mask);

    FakeTarget dim_only;
    outcome = portal_apply_patch(parse("{\"screen_dim_min\":0}"), portal_default_values(), dim_only);
    TEST_ASSERT_EQUAL_UINT32(1, dim_only.calls.size());
    TEST_ASSERT_EQUAL_UINT32(PF_SCREEN_DIM_MIN, dim_only.calls[0].index);
    TEST_ASSERT_EQUAL_UINT32(1u << PF_SCREEN_DIM_MIN, outcome.written_mask);
}

static void test_screen_timeouts_equal_to_the_state_are_not_written() {
    FakeTarget target;
    const PortalApplyOutcome outcome =
        portal_apply_patch(parse("{\"screen_dim_min\":5,\"screen_off_min\":60}"), portal_default_values(), target);
    TEST_ASSERT_EQUAL_UINT32(0, target.calls.size());  // no write, so no reload either
    TEST_ASSERT_EQUAL_UINT32(0, outcome.written_mask);
    // The comparison is per key: Never against the state's 60 is a change
    FakeTarget changed;
    portal_apply_patch(parse("{\"screen_dim_min\":5,\"screen_off_min\":0}"), portal_default_values(), changed);
    TEST_ASSERT_EQUAL_UINT32(1, changed.calls.size());
    TEST_ASSERT_EQUAL_UINT32(PF_SCREEN_OFF_MIN, changed.calls[0].index);
}

static void test_a_failing_screen_write_reports_its_key_and_keeps_the_earlier_ones() {
    FakeTarget target;
    target.fail_at = 1;  // swipe ok, screen_dim_min fails
    const PortalApplyOutcome outcome = portal_apply_patch(
        parse("{\"swipe\":true,\"screen_dim_min\":10,\"screen_off_min\":240,\"single_weight_g\":10}"), portal_default_values(), target);
    TEST_ASSERT_EQUAL(PortalError::STORE_FAILED, outcome.error);
    TEST_ASSERT_EQUAL_INT(PF_SCREEN_DIM_MIN, outcome.failed_field);
    TEST_ASSERT_EQUAL_UINT32(2, target.calls.size());
    TEST_ASSERT_EQUAL_UINT32(1u << PF_SWIPE, outcome.written_mask);
}

void run_portal_apply_tests() {
    RUN_TEST(test_screen_timeouts_reach_the_target_one_key_at_a_time);
    RUN_TEST(test_screen_timeouts_equal_to_the_state_are_not_written);
    RUN_TEST(test_a_failing_screen_write_reports_its_key_and_keeps_the_earlier_ones);
    RUN_TEST(test_the_address_flag_travels_from_outcome_to_reply);
    RUN_TEST(test_a_failed_write_result_carries_the_error);
    RUN_TEST(test_fields_are_written_in_index_order_and_the_name_last);
    RUN_TEST(test_unchanged_fields_are_not_written);
    RUN_TEST(test_a_patch_equal_to_the_state_writes_nothing);
    RUN_TEST(test_the_name_the_grinder_already_has_changes_nothing);
    RUN_TEST(test_the_name_is_offered_even_when_the_reported_name_matches);
    RUN_TEST(test_a_failed_write_stops_everything_after_it);
    RUN_TEST(test_the_name_only_changes_when_everything_else_was_stored);
    RUN_TEST(test_a_failing_name_write_keeps_the_fields_before_it);
    RUN_TEST(test_every_profile_field_reaches_the_target_with_its_value);
}
