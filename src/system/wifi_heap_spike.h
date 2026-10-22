#pragma once

// One-shot WiFi heap measurement (chg008 step 0). Only compiled with -DDEBUG_WIFI_HEAP_SPIKE=1.
// Starts its own task, waits ~30 s, runs the phases and logs "[SPIKE]" lines to Serial.
// Do not grind during the spike. The spike image contains the WiFi password in plaintext: do not share it.
void wifi_heap_spike_start();
