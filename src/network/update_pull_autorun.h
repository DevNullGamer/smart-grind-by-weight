#pragma once

// Dev/test only (DEBUG_PULL_AUTORUN, wifi-dev envs): drives the pull engine without any UI and logs the results.
//   1: ~25 s after boot (and a connected WiFi, an unpended image) Check, and if a newer release is offered, Install.
//      Run it against tools/wifi/pull_test_server.py. "[PULL][AUTORUN]" lines carry the verdicts.
//   2: HTTPS smoke test against the real Pages site: 3 x (firmware/index.json + discard-download of one .bin) with the
//      production TlsPsramScope, BLE left advertising; "[PULL][SMOKE]" lines carry sizes, rates and heap gates.
#include "../config/update.h"

#if DEBUG_PULL_AUTORUN
void update_pull_autorun_start();
#endif
