#include <unity.h>

void run_glink_plugin_messages_tests();
void run_glink_plugin_client_tests();
void run_glink_plugin_rx_ring_tests();
void run_glink_plugin_dosesave_tests();
void run_glink_plugin_devstate_tests();
void run_glink_plugin_settingssync_tests();
void run_glink_plugin_resultgate_tests();

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    run_glink_plugin_messages_tests();
    run_glink_plugin_client_tests();
    run_glink_plugin_rx_ring_tests();
    run_glink_plugin_dosesave_tests();
    run_glink_plugin_devstate_tests();
    run_glink_plugin_settingssync_tests();
    run_glink_plugin_resultgate_tests();
    return UNITY_END();
}
