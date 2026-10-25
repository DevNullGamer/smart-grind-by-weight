#pragma once
#include "grinder.h"

// Deadman bench check. Only compiled with -DDEBUG_GRINDER_DEADMAN_AUTORUN=1 (the ...-deadman-test env).
// Starts its own one-shot task ~6 s after boot that starts ONE continuous run on an initialized Grinder,
// waits for the 3 s test deadman and logs "[DEADMAN-TEST]" lines with the result. Needs nothing attached.
// With a grinder connected the motor really runs ~3 s on every boot of this env (disconnect it from mains).
// Drives only the pins the Grinder already drives; never GPIO18 on V2; only the primary pin is read.
void grinder_deadman_test_start(Grinder* grinder);
