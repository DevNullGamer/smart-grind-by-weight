#pragma once
#include <stdint.h>

// Pure decision helpers for grind safety timeouts and no-flow detection. No Arduino/ESP
// dependencies so they can be unit tested on the host. GrindController supplies the measurements
// and acts on the answers (abort_grind() or a fallback transition).

// True once limit_ms has passed since phase_start_ms. Wrap-safe; a start slightly ahead of now
// (ordering race) counts as zero elapsed.
bool grind_phase_timed_out(uint32_t now_ms, uint32_t phase_start_ms, uint32_t limit_ms);

// Prime phase delivered too little: check_after_ms into the phase the cup holds less than
// min_percent of the purge amount. Elapsed time is clamped like grind_phase_timed_out().
bool grind_prime_no_flow(uint32_t now_ms, uint32_t phase_start_ms, float weight_g, float purge_amount_g,
                         uint32_t check_after_ms, uint32_t min_percent);

// Predictive phase shows no flow: check_after_ms into the phase the weight gained since the
// lowest reading seen is below min_gain_g. Elapsed time is clamped like grind_phase_timed_out().
bool grind_predictive_no_flow(uint32_t now_ms, uint32_t phase_start_ms, float gain_g,
                              uint32_t check_after_ms, float min_gain_g);

// Mid-grind stall detection: the motor runs but the weight no longer reaches a new high.
// reference_g is the highest weight counted as a gain so far (never lowered); last_gain_ms is when
// it was last raised.
struct GrindStallTracker {
    float reference_g = 0.0f;
    uint32_t last_gain_ms = 0;
    bool has_reference = false;  // False until a finite weight was seen; the next finite reading then becomes the reference
};

// Start (or restart) tracking from the given weight at now_ms.
void grind_stall_reset(GrindStallTracker& t, uint32_t now_ms, float weight_g);

// Feed one reading; true when stalled. Motor off: re-baselines and returns false. Non-finite
// weight: ignored. A reading >= reference + min_gain_g raises the reference and restarts the
// window; otherwise stalled once window_ms has passed since the last gain (grind_phase_timed_out()
// semantics: wrap-safe, window 0 = stalled immediately).
bool grind_stall_update(GrindStallTracker& t, uint32_t now_ms, float weight_g, bool motor_on,
                        float min_gain_g, uint32_t window_ms);

// How long to wait for a pulse to report complete: pulse length plus margin (non-finite or
// negative lengths count as 0).
uint32_t grind_pulse_wait_limit_ms(float pulse_ms, uint32_t margin_ms);
