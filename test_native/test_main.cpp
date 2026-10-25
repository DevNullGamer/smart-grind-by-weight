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
void run_grind_safety_tests();
void run_grind_start_check_tests();

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
    run_grind_safety_tests();
    run_grind_start_check_tests();
    return UNITY_END();
}
