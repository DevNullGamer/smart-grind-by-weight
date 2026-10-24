#include "rmt_pulse_math.h"

uint32_t continuous_deadman_ms(uint32_t timeout_sec, uint32_t margin_ms) {
    const uint64_t total = (uint64_t)timeout_sec * 1000u + margin_ms;
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

size_t rmt_pulse_symbol_count(uint32_t high_us) {
    if (high_us == 0) return 0;
    const uint32_t halves = (high_us + RMT_MAX_HALF_US - 1) / RMT_MAX_HALF_US;  // HIGH halves
    return (halves + 2) / 2;  // ceil((halves + 1 tail half) / 2)
}

size_t rmt_build_pulse_symbols(uint32_t high_us, RmtSymbol* out, size_t capacity) {
    const size_t count = rmt_pulse_symbol_count(high_us);
    if (count == 0 || count > capacity || !out) return 0;

    // HIGH halves: all full length except the last, which carries the remainder (1..RMT_MAX_HALF_US)
    const uint32_t halves = (high_us + RMT_MAX_HALF_US - 1) / RMT_MAX_HALF_US;
    uint32_t remaining = high_us;
    size_t symbol = 0;
    uint32_t placed = 0;
    while (placed < halves) {
        RmtSymbol& s = out[symbol++];
        const uint32_t d0 = remaining > RMT_MAX_HALF_US ? RMT_MAX_HALF_US : remaining;
        remaining -= d0;
        s.level0 = 1;
        s.duration0 = (uint16_t)d0;
        placed++;
        if (placed < halves) {
            const uint32_t d1 = remaining > RMT_MAX_HALF_US ? RMT_MAX_HALF_US : remaining;
            remaining -= d1;
            s.level1 = 1;
            s.duration1 = (uint16_t)d1;
            placed++;
        } else {
            // Free second half: the LOW tail goes here
            s.level1 = 0;
            s.duration1 = (uint16_t)RMT_PULSE_TAIL_US;
            return symbol;
        }
    }

    // Both halves of the last symbol were HIGH: the tail gets its own symbol, second half unused
    out[symbol++] = RmtSymbol{0, (uint16_t)RMT_PULSE_TAIL_US, 0, 0};
    return symbol;
}
