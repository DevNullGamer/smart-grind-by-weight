#include "rmt_pulse_math.h"

uint32_t continuous_deadman_ms(uint32_t timeout_sec, uint32_t margin_ms) {
    const uint64_t total = (uint64_t)timeout_sec * 1000u + margin_ms;
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}
