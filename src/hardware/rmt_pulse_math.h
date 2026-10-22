#pragma once
#include <stdint.h>

// Pure helpers for motor RMT timing. No Arduino/ESP dependencies so they can be unit tested on the host.

// Time after which a continuous motor run is force-stopped by the hardware deadman:
// the controller timeout plus a margin so the normal timeout path always wins.
// Saturates at UINT32_MAX instead of overflowing.
uint32_t continuous_deadman_ms(uint32_t timeout_sec, uint32_t margin_ms);
