#include <unity.h>

void run_glink_plugin_messages_tests();
void run_glink_plugin_client_tests();
void run_glink_plugin_rx_ring_tests();

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    run_glink_plugin_messages_tests();
    run_glink_plugin_client_tests();
    run_glink_plugin_rx_ring_tests();
    return UNITY_END();
}
