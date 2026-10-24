#include "grind_safety.h"
#include <math.h>

bool grind_phase_timed_out(uint32_t now_ms, uint32_t phase_start_ms, uint32_t limit_ms) {
    const int32_t elapsed = (int32_t)(now_ms - phase_start_ms);
    return elapsed >= 0 && (uint32_t)elapsed >= limit_ms;
}

bool grind_prime_no_flow(uint32_t now_ms, uint32_t phase_start_ms, float weight_g, float purge_amount_g,
                         uint32_t check_after_ms, uint32_t min_percent) {
    if (!grind_phase_timed_out(now_ms, phase_start_ms, check_after_ms)) return false;
    return weight_g < purge_amount_g * (float)min_percent / 100.0f;
}

bool grind_predictive_no_flow(uint32_t now_ms, uint32_t phase_start_ms, float gain_g,
                              uint32_t check_after_ms, float min_gain_g) {
    if (!grind_phase_timed_out(now_ms, phase_start_ms, check_after_ms)) return false;
    return gain_g < min_gain_g;
}

uint32_t grind_pulse_wait_limit_ms(float pulse_ms, uint32_t margin_ms) {
    const float max_pulse_ms = 3600000.0f;  // One hour: far beyond any real pulse, keeps the cast defined
    if (!isfinite(pulse_ms) || pulse_ms < 0.0f) pulse_ms = 0.0f;
    if (pulse_ms > max_pulse_ms) pulse_ms = max_pulse_ms;
    return (uint32_t)pulse_ms + margin_ms;
}
