// GLinkDevState (chg025 3.4): devState classification and the profile-write gate.
#include <stdint.h>
#include <unity.h>
#include "GLinkDevState.h"

using namespace glinkc;

namespace {

void test_each_known_state_is_classified() {
    TEST_ASSERT_TRUE(classify_dev_state("IDLE") == DevClass::IDLE);
    TEST_ASSERT_TRUE(classify_dev_state("GRINDING") == DevClass::GRINDING);
    const char* busy[] = {"STARTING", "DONE", "SETTING", "UPDATING", "WARNING"};
    for (const char* s : busy) TEST_ASSERT_TRUE(classify_dev_state(s) == DevClass::BUSY);
}

void test_unknown_empty_null_and_case_variants_are_unknown() {
    TEST_ASSERT_TRUE(classify_dev_state("") == DevClass::UNKNOWN);
    TEST_ASSERT_TRUE(classify_dev_state(nullptr) == DevClass::UNKNOWN);
    TEST_ASSERT_TRUE(classify_dev_state("idle") == DevClass::UNKNOWN);
    TEST_ASSERT_TRUE(classify_dev_state("Grinding") == DevClass::UNKNOWN);
    TEST_ASSERT_TRUE(classify_dev_state("IDLE ") == DevClass::UNKNOWN);
    TEST_ASSERT_TRUE(classify_dev_state("IDL") == DevClass::UNKNOWN);
    TEST_ASSERT_TRUE(classify_dev_state("SOMETHING_NEW") == DevClass::UNKNOWN);
}

void test_accepts_profile_writes_only_idle_and_warning() {
    TEST_ASSERT_TRUE(accepts_profile_writes("IDLE"));
    TEST_ASSERT_TRUE(accepts_profile_writes("WARNING"));
    const char* no[] = {"GRINDING", "STARTING", "DONE", "SETTING", "UPDATING", "", "idle", "warning", "XYZ"};
    for (const char* s : no) TEST_ASSERT_FALSE(accepts_profile_writes(s));
    TEST_ASSERT_FALSE(accepts_profile_writes(nullptr));
}

}  // namespace

void run_glink_plugin_devstate_tests() {
    RUN_TEST(test_each_known_state_is_classified);
    RUN_TEST(test_unknown_empty_null_and_case_variants_are_unknown);
    RUN_TEST(test_accepts_profile_writes_only_idle_and_warning);
}
