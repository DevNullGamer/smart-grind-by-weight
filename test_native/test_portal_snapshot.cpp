#include <unity.h>
#include <math.h>
#include <string.h>
#include "network/portal_snapshot.h"
#include "network/wifi_credentials.h"

static void test_ui_state_names_are_unique_lower_case() {
    const UIState all[] = {UIState::READY,         UIState::GRINDING,   UIState::GRIND_COMPLETE, UIState::GRIND_TIMEOUT,
                           UIState::EDIT,          UIState::MENU,       UIState::CALIBRATION,    UIState::CONFIRM,
                           UIState::PURGE_CONFIRM, UIState::AUTOTUNING, UIState::OTA_UPDATE,     UIState::OTA_UPDATE_FAILED};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char* name = portal_ui_state_name(all[i]);
        TEST_ASSERT_TRUE_MESSAGE(strcmp(name, "unknown") != 0, name);
        for (const char* c = name; *c; c++) TEST_ASSERT_TRUE_MESSAGE((*c >= 'a' && *c <= 'z') || *c == '_', name);
        for (size_t j = 0; j < i; j++) TEST_ASSERT_TRUE(strcmp(name, portal_ui_state_name(all[j])) != 0);
    }
    TEST_ASSERT_EQUAL_STRING("ready", portal_ui_state_name(UIState::READY));
    TEST_ASSERT_EQUAL_STRING("grind_complete", portal_ui_state_name(UIState::GRIND_COMPLETE));
    TEST_ASSERT_EQUAL_STRING("ota_update_failed", portal_ui_state_name(UIState::OTA_UPDATE_FAILED));
}

static void test_grind_phase_names_follow_the_enum_order() {
    // GrindPhase (controllers/grind_controller.h) in declaration order
    const char* expected[] = {"idle",           "initializing",   "setup",          "taring",
                              "tare_confirm",   "predictive",     "pulse_decision", "pulse_execute",
                              "pulse_settling", "final_settling", "time_grinding",  "time_additional_pulse",
                              "completed",      "timeout",        "prime",          "prime_settling",
                              "purge_confirm"};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected) / sizeof(expected[0]), PORTAL_GRIND_PHASE_COUNT);
    for (size_t i = 0; i < PORTAL_GRIND_PHASE_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(expected[i], portal_grind_phase_name(static_cast<int>(i)));
    }
    TEST_ASSERT_EQUAL_STRING("unknown", portal_grind_phase_name(-1));
    TEST_ASSERT_EQUAL_STRING("unknown", portal_grind_phase_name(static_cast<int>(PORTAL_GRIND_PHASE_COUNT)));
}

static void test_ui_mode_needs_the_home_screens_without_an_overlay() {
    TEST_ASSERT_EQUAL(PortalUiMode::READY, portal_ui_mode(UIState::READY, false));
    TEST_ASSERT_EQUAL(PortalUiMode::MENU, portal_ui_mode(UIState::MENU, false));
    TEST_ASSERT_EQUAL(PortalUiMode::OTHER, portal_ui_mode(UIState::READY, true));
    TEST_ASSERT_EQUAL(PortalUiMode::OTHER, portal_ui_mode(UIState::MENU, true));
    const UIState others[] = {UIState::GRINDING,   UIState::GRIND_COMPLETE, UIState::GRIND_TIMEOUT, UIState::EDIT,
                              UIState::CALIBRATION, UIState::CONFIRM,        UIState::PURGE_CONFIRM, UIState::AUTOTUNING,
                              UIState::OTA_UPDATE,  UIState::OTA_UPDATE_FAILED};
    for (UIState s : others) {
        TEST_ASSERT_EQUAL(PortalUiMode::OTHER, portal_ui_mode(s, false));
        TEST_ASSERT_EQUAL(PortalUiMode::OTHER, portal_ui_mode(s, true));
    }
}

static void test_status_is_built_from_the_inputs() {
    PortalStatusInputs in;
    in.ui_state = UIState::MENU;
    in.grind_phase_id = 5;
    in.busy = true;
    in.weight_valid = true;
    in.weight_g = 12.5f;
    in.motor_available = true;
    in.start_block = GrindStartBlock::LOAD_CELL_NO_DATA;
    in.motor_test_block = MotorTestBlock::MOTOR_FAULT;
    in.uptime_s = 321;
    in.build = 504;
    const PortalStatus s = portal_build_status(in);
    TEST_ASSERT_EQUAL_STRING("menu", s.ui_state);
    TEST_ASSERT_EQUAL_STRING("predictive", s.grind_phase);
    TEST_ASSERT_TRUE(s.busy);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, s.weight_g);
    TEST_ASSERT_TRUE(s.motor_available);
    TEST_ASSERT_EQUAL(PortalError::LOAD_CELL_NO_DATA, s.start_block);
    TEST_ASSERT_EQUAL(PortalError::MOTOR_FAULT, s.motor_test_block);
    TEST_ASSERT_EQUAL_UINT32(321, s.uptime_s);
    TEST_ASSERT_EQUAL_INT32(504, s.build);
    TEST_ASSERT_TRUE(s.web_interface);

    char json[PORTAL_STATUS_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_status(s, json, sizeof(json)));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"ui_state\":\"menu\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"start_block\":\"load_cell_no_data\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"motor_test_block\":\"motor_fault\""));
}

static void test_status_defaults_are_all_clear() {
    const PortalStatus s = portal_build_status(PortalStatusInputs());
    TEST_ASSERT_EQUAL_STRING("ready", s.ui_state);
    TEST_ASSERT_EQUAL_STRING("idle", s.grind_phase);
    TEST_ASSERT_EQUAL(PortalError::NONE, s.start_block);
    TEST_ASSERT_EQUAL(PortalError::NONE, s.motor_test_block);
    char json[PORTAL_STATUS_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_status(s, json, sizeof(json)));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"start_block\":null"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"weight_g\":null"));  // no valid reading by default
}

static void test_an_invalid_weight_is_null() {
    PortalStatusInputs in;
    in.weight_valid = false;
    in.weight_g = 12.0f;
    TEST_ASSERT_TRUE(isnan(portal_build_status(in).weight_g));
    in.weight_valid = true;
    in.weight_g = NAN;
    TEST_ASSERT_TRUE(isnan(portal_build_status(in).weight_g));
    in.weight_g = INFINITY;
    TEST_ASSERT_TRUE(isnan(portal_build_status(in).weight_g));
    in.weight_g = -0.4f;  // a slightly negative scale reading is a valid reading
    TEST_ASSERT_EQUAL_FLOAT(-0.4f, portal_build_status(in).weight_g);
}

static void test_status_json_fits_its_buffer_in_the_worst_case() {
    PortalStatusInputs in;
    in.ui_state = UIState::OTA_UPDATE_FAILED;
    in.grind_phase_id = 11;  // time_additional_pulse, the longest name
    in.busy = true;
    in.weight_valid = true;
    in.weight_g = -1000.123f;
    in.motor_available = true;
    in.start_block = GrindStartBlock::LOAD_CELL_NOT_CONNECTED;
    in.motor_test_block = MotorTestBlock::UPDATE_IN_PROGRESS;
    in.uptime_s = 4294967295u;
    in.build = 2147483647;
    char json[PORTAL_STATUS_JSON_MAX];
    TEST_ASSERT_TRUE(portal_encode_status(portal_build_status(in), json, sizeof(json)));
}

static void test_replies_fit_their_buffers_in_the_worst_case() {
    static char buf[PORTAL_SCHEMA_JSON_MAX];
    PortalValues values = portal_default_values();
    for (size_t i = 0; i < PORTAL_FIELD_COUNT; i++) values.v[i] = (i < PF_BRIGHTNESS_NORMAL) ? 1.0f : 1000.0f;
    values.v[PF_PURGE_MODE] = 1;
    values.v[PF_GRIND_MODE] = 1;
    values.v[PF_SINGLE_WEIGHT_G] = 999.999f;
    memset(values.device_name, 'a', PORTAL_DEVICE_NAME_MAX_LEN);
    values.device_name[PORTAL_DEVICE_NAME_MAX_LEN] = '\0';
    TEST_ASSERT_TRUE(portal_encode_values(values, buf, PORTAL_VALUES_JSON_MAX));
    TEST_ASSERT_TRUE(portal_encode_patch_reply(values, (1u << PORTAL_FIELD_COUNT) - 1, buf, PORTAL_VALUES_JSON_MAX, true));
    TEST_ASSERT_TRUE(portal_encode_schema(buf, PORTAL_SCHEMA_JSON_MAX));
    // The documented sizes keep some headroom
    TEST_ASSERT_TRUE(strlen(buf) + 256 < PORTAL_SCHEMA_JSON_MAX);
}

static void test_device_name_from_host() {
    char out[PORTAL_DEVICE_NAME_MAX_LEN + 1];
    portal_device_name_from_host("grindbyweight", out);
    TEST_ASSERT_EQUAL_STRING("grindbyweight", out);

    // exactly at the limit: unchanged
    const char* at_limit = "abcdefghijklmnopqrstuvwx";  // 24
    portal_device_name_from_host(at_limit, out);
    TEST_ASSERT_EQUAL_STRING(at_limit, out);

    // longer: cut to 24, and the result is always a name the API accepts
    portal_device_name_from_host("abcdefghijklmnopqrstuvwxyz0123456789", out);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwx", out);
    TEST_ASSERT_TRUE(portal_device_name_valid(out));

    // the cut lands on hyphens: they are trimmed, never leaving a name the validator refuses
    portal_device_name_from_host("abcdefghijklmnopqrstuv--yz0123456789", out);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuv", out);
    TEST_ASSERT_TRUE(portal_device_name_valid(out));

    // 63 characters (the WiFiManager limit)
    char longest[64];
    memset(longest, 'a', 63);
    longest[63] = '\0';
    portal_device_name_from_host(longest, out);
    TEST_ASSERT_EQUAL_UINT32(PORTAL_DEVICE_NAME_MAX_LEN, strlen(out));
    TEST_ASSERT_TRUE(portal_device_name_valid(out));
    TEST_ASSERT_TRUE(wifi_validate_hostname(longest));  // it is a name WiFiManager accepts

    portal_device_name_from_host(nullptr, out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void run_portal_snapshot_tests() {
    RUN_TEST(test_ui_state_names_are_unique_lower_case);
    RUN_TEST(test_grind_phase_names_follow_the_enum_order);
    RUN_TEST(test_ui_mode_needs_the_home_screens_without_an_overlay);
    RUN_TEST(test_status_is_built_from_the_inputs);
    RUN_TEST(test_status_defaults_are_all_clear);
    RUN_TEST(test_an_invalid_weight_is_null);
    RUN_TEST(test_status_json_fits_its_buffer_in_the_worst_case);
    RUN_TEST(test_replies_fit_their_buffers_in_the_worst_case);
    RUN_TEST(test_device_name_from_host);
}
