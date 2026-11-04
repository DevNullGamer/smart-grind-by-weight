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

void grind_stall_reset(GrindStallTracker& t, uint32_t now_ms, float weight_g) {
    t.has_reference = isfinite(weight_g);
    t.reference_g = t.has_reference ? weight_g : 0.0f;
    t.last_gain_ms = now_ms;
}

bool grind_stall_update(GrindStallTracker& t, uint32_t now_ms, float weight_g, bool motor_on,
                        float min_gain_g, uint32_t window_ms) {
    if (!motor_on) {
        grind_stall_reset(t, now_ms, weight_g);
        return false;
    }
    if (!isfinite(weight_g)) return false;
    if (!t.has_reference) {  // Reset saw a non-finite weight: the first finite reading becomes the reference
        grind_stall_reset(t, now_ms, weight_g);
        return false;
    }
    if (weight_g >= t.reference_g + min_gain_g) {
        t.reference_g = weight_g;
        t.last_gain_ms = now_ms;
        return false;
    }
    return grind_phase_timed_out(now_ms, t.last_gain_ms, window_ms);
}

uint32_t grind_pulse_wait_limit_ms(float pulse_ms, uint32_t margin_ms) {
    const float max_pulse_ms = 3600000.0f;  // One hour: far beyond any real pulse, keeps the cast defined
    if (!isfinite(pulse_ms) || pulse_ms < 0.0f) pulse_ms = 0.0f;
    if (pulse_ms > max_pulse_ms) pulse_ms = max_pulse_ms;
    return (uint32_t)pulse_ms + margin_ms;
}
