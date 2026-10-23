// Single Unity entry point: PlatformIO 6.2 loses the -c project config after the first test
// suite of a run, so all host tests live in one suite and register themselves here.
#include <unity.h>

void run_board_decide_tests();
void run_rmt_pulse_math_tests();
void run_wifi_credentials_tests();
void run_image_header_tests();
void run_sha_hex_tests();

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    run_board_decide_tests();
    run_rmt_pulse_math_tests();
    run_wifi_credentials_tests();
    run_image_header_tests();
    run_sha_hex_tests();
    return UNITY_END();
}
