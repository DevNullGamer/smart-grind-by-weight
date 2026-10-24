#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure helpers for motor RMT timing. No Arduino/ESP dependencies so they can be unit tested on the host.

// Time after which a continuous motor run is force-stopped by the hardware deadman:
// the controller timeout plus a margin so the normal timeout path always wins.
// Saturates at UINT32_MAX instead of overflowing.
uint32_t continuous_deadman_ms(uint32_t timeout_sec, uint32_t margin_ms);

// ---- Exact finite pulses -------------------------------------------------------------------
// A pulse is sent as ONE plain RMT payload (no loop_count): the HIGH time is split into
// halves of at most RMT_MAX_HALF_US (the 15-bit RMT duration field), packed two per symbol,
// followed by a 1 us LOW tail. Looping a payload repeats its remainder and tail on every pass,
// which made long pulses come out wrong (measured 100 ms -> 69 ms, 1000 ms -> 1443 ms).
// The HIGH time is exact; the tail comes on top, as it always did for short pulses.

constexpr uint32_t RMT_MAX_HALF_US = 32767;      // Longest single level duration (15 bits)
constexpr uint32_t RMT_PULSE_TAIL_US = 1;        // LOW tail after the HIGH time
constexpr size_t RMT_PULSE_SYMBOL_CAPACITY = 64; // Payload buffer size; 4000 ms needs 62

struct RmtSymbol {
    uint8_t level0;
    uint16_t duration0;  // 0 only for the unused second half of the final tail symbol (end marker)
    uint8_t level1;
    uint16_t duration1;
};

// Number of symbols rmt_build_pulse_symbols() produces for high_us, 0 if high_us is 0.
size_t rmt_pulse_symbol_count(uint32_t high_us);

// Fills out[] with the payload for a pulse that is HIGH for exactly high_us, then LOW for
// RMT_PULSE_TAIL_US. Returns the symbol count, or 0 (out untouched) if high_us is 0 or the
// payload would not fit in capacity.
size_t rmt_build_pulse_symbols(uint32_t high_us, RmtSymbol* out, size_t capacity);
