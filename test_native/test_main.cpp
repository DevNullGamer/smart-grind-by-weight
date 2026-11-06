// Single Unity entry point: PlatformIO 6.2 loses the -c project config after the first test
// suite of a run, so all host tests live in one suite and register themselves here.
#include <unity.h>

void run_board_decide_tests();
void run_rmt_pulse_math_tests();
void run_wifi_credentials_tests();
void run_image_header_tests();
void run_sha_hex_tests();
void run_update_http_logic_tests();
void run_wifi_status_text_tests();
void run_update_approval_tests();
void run_update_prompt_text_tests();
void run_update_ui_logic_tests();
void run_boot_health_tests();
void run_release_index_tests();
void run_pull_logic_tests();
void run_pull_ui_text_tests();
void run_sensor_health_tests();
void run_circular_buffer_math_tests();
void run_calibration_math_tests();
void run_grind_safety_tests();
void run_grind_start_check_tests();
void run_motor_test_check_tests();
void run_ui_format_tests();
void run_ui_settings_tests();
void run_menu_setting_models_tests();
void run_menu_setting_table_tests();
void run_menu_setting_sliders_tests();
void run_menu_setting_radios_tests();
void run_portal_api_tests();
void run_portal_guard_tests();
void run_portal_page_logic_tests();
void run_portal_mailbox_tests();
void run_portal_apply_tests();
void run_portal_snapshot_tests();
void run_grind_history_tests();
void run_grind_history_store_tests();
void run_portal_insights_tests();
void run_screen_power_tests();
void run_glink_frame_tests();
void run_glink_json_tests();
void run_glink_messages_tests();
void run_glink_decide_tests();
void run_grind_size_tests();

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    run_board_decide_tests();
    run_rmt_pulse_math_tests();
    run_wifi_credentials_tests();
    run_image_header_tests();
    run_sha_hex_tests();
    run_update_http_logic_tests();
    run_wifi_status_text_tests();
    run_update_approval_tests();
    run_update_prompt_text_tests();
    run_update_ui_logic_tests();
    run_boot_health_tests();
    run_release_index_tests();
    run_pull_logic_tests();
    run_pull_ui_text_tests();
    run_sensor_health_tests();
    run_circular_buffer_math_tests();
    run_calibration_math_tests();
    run_grind_safety_tests();
    run_grind_start_check_tests();
    run_motor_test_check_tests();
    run_ui_format_tests();
    run_ui_settings_tests();
    run_menu_setting_models_tests();
    run_menu_setting_table_tests();
    run_menu_setting_sliders_tests();
    run_menu_setting_radios_tests();
    run_portal_api_tests();
    run_portal_guard_tests();
    run_portal_page_logic_tests();
    run_portal_mailbox_tests();
    run_portal_apply_tests();
    run_portal_snapshot_tests();
    run_grind_history_tests();
    run_grind_history_store_tests();
    run_portal_insights_tests();
    run_screen_power_tests();
    run_glink_frame_tests();
    run_glink_json_tests();
    run_glink_messages_tests();
    run_glink_decide_tests();
    run_grind_size_tests();
    return UNITY_END();
}
