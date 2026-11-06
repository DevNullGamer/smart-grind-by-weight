#include <unity.h>
#include <limits>
#include "controllers/grind_safety.h"
#include "config/grind_control.h"
#include "bluetooth/glink_results.h"
#include "bluetooth/glink_session_map.h"
#include <fstream>
#include <string>
#include <vector>

static void test_timed_out_normal() {
    TEST_ASSERT_FALSE(grind_phase_timed_out(1500, 1000, 1000));
    TEST_ASSERT_TRUE(grind_phase_timed_out(3000, 1000, 1000));
}

static void test_timed_out_boundary() {
    TEST_ASSERT_FALSE(grind_phase_timed_out(1999, 1000, 1000));
    TEST_ASSERT_TRUE(grind_phase_timed_out(2000, 1000, 1000));
    TEST_ASSERT_TRUE(grind_phase_timed_out(2001, 1000, 1000));
}

static void test_timed_out_now_before_start() {
    // start ahead of now counts as 0 elapsed: only a zero limit fires
    TEST_ASSERT_FALSE(grind_phase_timed_out(1000, 1005, 1));
    TEST_ASSERT_FALSE(grind_phase_timed_out(1000, 1005, 5000));
    // across the wrap as well
    TEST_ASSERT_FALSE(grind_phase_timed_out(0xFFFFFFFEu, 3u, 1));
}

static void test_timed_out_wrap() {
    // start 0xFFFFFF00, now 0x000000FF: 511 ms elapsed
    TEST_ASSERT_FALSE(grind_phase_timed_out(0x000000FFu, 0xFFFFFF00u, 512));
    TEST_ASSERT_TRUE(grind_phase_timed_out(0x000000FFu, 0xFFFFFF00u, 511));
    TEST_ASSERT_TRUE(grind_phase_timed_out(0x000000FFu, 0xFFFFFF00u, 100));
}

static void test_prime_no_flow_before_check_time() {
    TEST_ASSERT_FALSE(grind_prime_no_flow(5999, 1000, 0.0f, 1.0f, 5000, 50));
    TEST_ASSERT_FALSE(grind_prime_no_flow(1000, 1000, 0.0f, 1.0f, 5000, 50));
}

static void test_prime_no_flow_at_check_time() {
    TEST_ASSERT_TRUE(grind_prime_no_flow(6000, 1000, 0.0f, 1.0f, 5000, 50));
    TEST_ASSERT_TRUE(grind_prime_no_flow(9000, 1000, 0.49f, 1.0f, 5000, 50));
}

static void test_prime_no_flow_weight_threshold() {
    // exactly 50% of 1.0 g is enough
    TEST_ASSERT_FALSE(grind_prime_no_flow(6000, 1000, 0.5f, 1.0f, 5000, 50));
    TEST_ASSERT_FALSE(grind_prime_no_flow(6000, 1000, 0.51f, 1.0f, 5000, 50));
    TEST_ASSERT_TRUE(grind_prime_no_flow(6000, 1000, 0.49f, 1.0f, 5000, 50));
    // scales with purge amount (2.5 g -> 1.25 g)
    TEST_ASSERT_TRUE(grind_prime_no_flow(6000, 1000, 1.2f, 2.5f, 5000, 50));
    TEST_ASSERT_FALSE(grind_prime_no_flow(6000, 1000, 1.3f, 2.5f, 5000, 50));
}

static void test_prime_no_flow_start_ahead_of_now() {
    TEST_ASSERT_FALSE(grind_prime_no_flow(1000, 1005, 0.0f, 1.0f, 5000, 50));
}

static void test_prime_no_flow_uses_config_constants() {
    TEST_ASSERT_EQUAL_UINT32(5000, GRIND_PRIME_NO_FLOW_CHECK_MS);
    TEST_ASSERT_EQUAL_UINT32(50, GRIND_PRIME_NO_FLOW_MIN_PERCENT);
    TEST_ASSERT_TRUE(grind_prime_no_flow(5000, 0, 0.0f, 1.0f, GRIND_PRIME_NO_FLOW_CHECK_MS,
                                         GRIND_PRIME_NO_FLOW_MIN_PERCENT));
    TEST_ASSERT_FALSE(grind_prime_no_flow(4999, 0, 0.0f, 1.0f, GRIND_PRIME_NO_FLOW_CHECK_MS,
                                          GRIND_PRIME_NO_FLOW_MIN_PERCENT));
}

static void test_predictive_no_flow_before_check_time() {
    TEST_ASSERT_FALSE(grind_predictive_no_flow(4999, 1000, 0.0f, 4000, 0.3f));  // 3999 elapsed
    TEST_ASSERT_FALSE(grind_predictive_no_flow(1000, 1000, 0.0f, 4000, 0.3f));
}

static void test_predictive_no_flow_gain_boundary() {
    TEST_ASSERT_TRUE(grind_predictive_no_flow(5000, 1000, 0.0f, 4000, 0.3f));
    TEST_ASSERT_TRUE(grind_predictive_no_flow(5000, 1000, 0.29f, 4000, 0.3f));
    TEST_ASSERT_FALSE(grind_predictive_no_flow(5000, 1000, 0.3f, 4000, 0.3f));
    TEST_ASSERT_FALSE(grind_predictive_no_flow(5000, 1000, 0.31f, 4000, 0.3f));
    TEST_ASSERT_FALSE(grind_predictive_no_flow(9000, 1000, 5.0f, 4000, 0.3f));
}

static void test_predictive_no_flow_start_ahead_of_now() {
    TEST_ASSERT_FALSE(grind_predictive_no_flow(1000, 1005, 0.0f, 4000, 0.3f));
}

static void test_predictive_no_flow_config_constants() {
    TEST_ASSERT_EQUAL_UINT32(4000, GRIND_PREDICTIVE_NO_FLOW_CHECK_MS);
    TEST_ASSERT_EQUAL_FLOAT(0.3f, GRIND_PREDICTIVE_NO_FLOW_MIN_GAIN_G);
}

static void test_pulse_wait_limit_normal() {
    TEST_ASSERT_EQUAL_UINT32(100 + GRIND_PULSE_WAIT_MARGIN_MS,
                             grind_pulse_wait_limit_ms(100.0f, GRIND_PULSE_WAIT_MARGIN_MS));
    TEST_ASSERT_EQUAL_UINT32(1000, GRIND_PULSE_WAIT_MARGIN_MS);
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(0.0f, 1000));
    TEST_ASSERT_EQUAL_UINT32(350, grind_pulse_wait_limit_ms(250.0f, 100));
}

static void test_pulse_wait_limit_invalid_inputs() {
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    const float inf_v = std::numeric_limits<float>::infinity();
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(nan_v, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(-50.0f, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(-inf_v, 1000));
    // +infinity is non-finite -> counts as 0
    TEST_ASSERT_EQUAL_UINT32(1000, grind_pulse_wait_limit_ms(inf_v, 1000));
}

static void test_pulse_wait_limit_huge_clamped() {
    TEST_ASSERT_EQUAL_UINT32(3600000u + 1000u, grind_pulse_wait_limit_ms(1.0e12f, 1000));
    TEST_ASSERT_EQUAL_UINT32(3600000u + 1000u, grind_pulse_wait_limit_ms(3.0e38f, 1000));
    TEST_ASSERT_EQUAL_UINT32(3600000u, grind_pulse_wait_limit_ms(3600000.0f, 0));
}

// ---- Stall tracker (chg019) ----
static const float STALL_GAIN = GRIND_STALL_MIN_GAIN_G;
static const uint32_t STALL_WIN = GRIND_STALL_WINDOW_MS;

static bool stall_upd(GrindStallTracker& t, uint32_t now, float w, bool motor = true) {
    return grind_stall_update(t, now, w, motor, STALL_GAIN, STALL_WIN);
}

static void test_stall_constants() {
    TEST_ASSERT_EQUAL_UINT32(5000, GRIND_STALL_WINDOW_MS);
    TEST_ASSERT_EQUAL_FLOAT(0.3f, GRIND_STALL_MIN_GAIN_G);
}

static void test_stall_window_boundary() {
    GrindStallTracker t;
    grind_stall_reset(t, 1000, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 1000, 5.0f));
    TEST_ASSERT_FALSE(stall_upd(t, 1000 + 4999, 5.0f));
    TEST_ASSERT_TRUE(stall_upd(t, 1000 + 5000, 5.0f));
    TEST_ASSERT_TRUE(stall_upd(t, 1000 + 9000, 5.0f));
}

static void test_stall_gain_exact_resets() {
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    const float exact = 5.0f + GRIND_STALL_MIN_GAIN_G;
    TEST_ASSERT_FALSE(stall_upd(t, 4000, exact));  // gain of exactly the threshold
    TEST_ASSERT_EQUAL_FLOAT(exact, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(4000, t.last_gain_ms);
    TEST_ASSERT_FALSE(stall_upd(t, 4000 + 4999, exact));  // window restarted at 4000
    TEST_ASSERT_TRUE(stall_upd(t, 4000 + 5000, exact));
}

static void test_stall_gain_just_below_does_not_reset() {
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    const float below = 5.0f + 0.29f;
    TEST_ASSERT_FALSE(stall_upd(t, 4000, below));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(0, t.last_gain_ms);
    TEST_ASSERT_TRUE(stall_upd(t, 5000, below));
}

static void test_stall_reference_moves_up_only() {
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 1000, 6.0f));  // reference -> 6.0, window at 1000
    TEST_ASSERT_FALSE(stall_upd(t, 2000, 4.0f));  // dip: reference stays
    TEST_ASSERT_EQUAL_FLOAT(6.0f, t.reference_g);
    TEST_ASSERT_FALSE(stall_upd(t, 3000, 6.0f));  // back at old level: no reset
    TEST_ASSERT_EQUAL_UINT32(1000, t.last_gain_ms);
    TEST_ASSERT_FALSE(stall_upd(t, 1000 + 4999, 6.0f));
    TEST_ASSERT_TRUE(stall_upd(t, 1000 + 5000, 6.0f));
}

static void test_stall_bump_resets_once() {
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    const float bump = 6.0f + GRIND_STALL_MIN_GAIN_G;
    TEST_ASSERT_FALSE(stall_upd(t, 3000, 6.0f));
    TEST_ASSERT_FALSE(stall_upd(t, 4000, bump));  // above reference + gain: reset
    TEST_ASSERT_EQUAL_UINT32(4000, t.last_gain_ms);
    TEST_ASSERT_FALSE(stall_upd(t, 4500, 5.0f));  // knocked down again: no further reset
    TEST_ASSERT_EQUAL_UINT32(4000, t.last_gain_ms);
    TEST_ASSERT_TRUE(stall_upd(t, 4000 + 5000, bump - 1.0f));
}

static void test_stall_motor_off_rebaselines() {
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 10000, 5.0f, false));  // motor off, would be long stalled
    TEST_ASSERT_EQUAL_UINT32(10000, t.last_gain_ms);
    TEST_ASSERT_FALSE(stall_upd(t, 10001, 5.0f, true));   // no instant abort
    TEST_ASSERT_FALSE(stall_upd(t, 10000 + 4999, 5.0f, true));
    TEST_ASSERT_TRUE(stall_upd(t, 10000 + 5000, 5.0f, true));
}

static void test_stall_motor_off_rebaselines_weight() {
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 100, 9.0f, false));
    TEST_ASSERT_EQUAL_FLOAT(9.0f, t.reference_g);
    stall_upd(t, 200, 8.0f, false);  // motor-off may lower the reference (re-baseline)
    TEST_ASSERT_EQUAL_FLOAT(8.0f, t.reference_g);
}

static void test_stall_non_finite_ignored() {
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    const float inf_v = std::numeric_limits<float>::infinity();
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 6000, nan_v));  // window passed, but reading ignored
    TEST_ASSERT_FALSE(stall_upd(t, 6000, inf_v));
    TEST_ASSERT_FALSE(stall_upd(t, 6000, -inf_v));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(0, t.last_gain_ms);
    TEST_ASSERT_TRUE(stall_upd(t, 6000, 5.0f));  // finite reading still sees the stall
}

static void test_stall_now_before_last_gain() {
    GrindStallTracker t;
    grind_stall_reset(t, 10000, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 9000, 5.0f));
    TEST_ASSERT_FALSE(stall_upd(t, 0, 5.0f));
    TEST_ASSERT_FALSE(stall_upd(t, 10000 - 1, 5.0f));
}

static void test_stall_wrap() {
    const uint32_t start = 0xFFFFFFFFu - 1000u;
    GrindStallTracker t;
    grind_stall_reset(t, start, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, start + 4999u, 5.0f));  // wraps past 0
    TEST_ASSERT_TRUE(stall_upd(t, start + 5000u, 5.0f));
    // gain across the wrap restarts the window
    TEST_ASSERT_FALSE(stall_upd(t, start + 3000u, 5.0f + GRIND_STALL_MIN_GAIN_G));
    TEST_ASSERT_FALSE(stall_upd(t, start + 3000u + 4999u, 5.0f + GRIND_STALL_MIN_GAIN_G));
    TEST_ASSERT_TRUE(stall_upd(t, start + 3000u + 5000u, 5.0f + GRIND_STALL_MIN_GAIN_G));
}

static void test_stall_window_zero() {
    GrindStallTracker t;
    grind_stall_reset(t, 500, 5.0f);
    TEST_ASSERT_TRUE(grind_stall_update(t, 500, 5.0f, true, STALL_GAIN, 0));
}

static void test_stall_slow_rise_aborts() {
    // 0.2 g per 5 s stays under the 0.3 g threshold against the fixed reference
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    bool stalled = false;
    for (uint32_t ms = 500; ms <= 5000 && !stalled; ms += 500)
        stalled = stall_upd(t, ms, 5.0f + 0.2f * (float)ms / 5000.0f);
    TEST_ASSERT_TRUE(stalled);
}

static void test_stall_fast_rise_does_not_abort() {
    // 0.4 g per 5 s = 0.04 g per 500 ms; the reference only moves once 0.3 g accumulated
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    for (uint32_t ms = 500; ms <= 60000; ms += 500)
        TEST_ASSERT_FALSE(stall_upd(t, ms, 5.0f + 0.4f * (float)ms / 5000.0f));
}

static void test_stall_nan_at_reset_then_rising() {
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    const float inf_v = std::numeric_limits<float>::infinity();
    GrindStallTracker t;
    grind_stall_reset(t, 100, nan_v);
    TEST_ASSERT_FALSE(t.has_reference);
    TEST_ASSERT_FALSE(stall_upd(t, 200, nan_v));  // still no reference
    TEST_ASSERT_FALSE(t.has_reference);
    // first finite reading becomes the reference far past the window: no false abort
    TEST_ASSERT_FALSE(stall_upd(t, 9000, 3.0f));
    TEST_ASSERT_TRUE(t.has_reference);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(9000, t.last_gain_ms);
    TEST_ASSERT_FALSE(stall_upd(t, 9000 + 4999, 3.0f));
    TEST_ASSERT_TRUE(stall_upd(t, 9000 + 5000, 3.0f));
    // infinity at reset behaves the same
    grind_stall_reset(t, 0, inf_v);
    TEST_ASSERT_FALSE(t.has_reference);
    TEST_ASSERT_FALSE(stall_upd(t, 20000, 7.0f));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(20000, t.last_gain_ms);
    // rising weight after a NaN reset: no abort
    grind_stall_reset(t, 0, nan_v);
    TEST_ASSERT_FALSE(stall_upd(t, 7000, 1.0f));
    TEST_ASSERT_FALSE(stall_upd(t, 7500, 1.0f + GRIND_STALL_MIN_GAIN_G));
    TEST_ASSERT_FALSE(stall_upd(t, 7500 + 4999, 1.0f + GRIND_STALL_MIN_GAIN_G));
}

static void test_stall_motor_off_non_finite_then_on() {
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    GrindStallTracker t;
    grind_stall_reset(t, 0, 5.0f);
    TEST_ASSERT_FALSE(stall_upd(t, 10000, nan_v, false));  // goes through reset
    TEST_ASSERT_FALSE(t.has_reference);
    TEST_ASSERT_FALSE(stall_upd(t, 20000, 4.0f, true));    // first finite becomes reference
    TEST_ASSERT_EQUAL_FLOAT(4.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(20000, t.last_gain_ms);
    TEST_ASSERT_FALSE(stall_upd(t, 24999, 4.0f));
    TEST_ASSERT_TRUE(stall_upd(t, 25000, 4.0f));
}

static void test_stall_default_initialised() {
    GrindStallTracker t;
    TEST_ASSERT_FALSE(t.has_reference);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(0, t.last_gain_ms);
    // Motor on without reset: first finite reading is the reference, never an abort
    TEST_ASSERT_FALSE(stall_upd(t, 1000000, 12.0f));
    TEST_ASSERT_TRUE(t.has_reference);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, t.reference_g);
    TEST_ASSERT_EQUAL_UINT32(1000000, t.last_gain_ms);
    GrindStallTracker u;
    TEST_ASSERT_FALSE(stall_upd(u, 1000000, std::numeric_limits<float>::quiet_NaN()));
    TEST_ASSERT_FALSE(u.has_reference);
}

// Abort reason of every safety bit (chg020 step 3): one bit, one reason; anything else is NONE
static void test_abort_reason_every_safety_bit() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::SENSOR, (uint8_t)grind_abort_reason_from_safety_bit(GRIND_SAFETY_REASON_SENSOR));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::TARE, (uint8_t)grind_abort_reason_from_safety_bit(GRIND_SAFETY_REASON_TARE));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::PRIME_FLOW, (uint8_t)grind_abort_reason_from_safety_bit(GRIND_SAFETY_REASON_PRIME_FLOW));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::PREDICTIVE_FLOW, (uint8_t)grind_abort_reason_from_safety_bit(GRIND_SAFETY_REASON_PREDICTIVE_FLOW));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::STALL, (uint8_t)grind_abort_reason_from_safety_bit(GRIND_SAFETY_REASON_STALL));
}

static void test_abort_reason_safety_bits_are_distinct_single_bits() {
    const uint8_t bits[] = {GRIND_SAFETY_REASON_SENSOR, GRIND_SAFETY_REASON_TARE, GRIND_SAFETY_REASON_PRIME_FLOW,
                            GRIND_SAFETY_REASON_PREDICTIVE_FLOW, GRIND_SAFETY_REASON_STALL};
    uint8_t seen = 0;
    for (uint8_t b : bits) {
        TEST_ASSERT_NOT_EQUAL(0, b);
        TEST_ASSERT_EQUAL_UINT8(0, b & (b - 1));  // exactly one bit
        TEST_ASSERT_EQUAL_UINT8(0, seen & b);
        seen |= b;
        TEST_ASSERT_NOT_EQUAL((uint8_t)GrindAbortReason::NONE, (uint8_t)grind_abort_reason_from_safety_bit(b));
    }
}

static void test_abort_reason_unknown_or_combined_bits_are_none() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::NONE, (uint8_t)grind_abort_reason_from_safety_bit(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::NONE,
                            (uint8_t)grind_abort_reason_from_safety_bit(GRIND_SAFETY_REASON_SENSOR | GRIND_SAFETY_REASON_TARE));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::NONE, (uint8_t)grind_abort_reason_from_safety_bit(1 << 5));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GrindAbortReason::NONE, (uint8_t)grind_abort_reason_from_safety_bit(0x80));
}

// GrindAbortReason maps value for value onto the GLink mirror enum
static void test_abort_reason_maps_to_glink_mirror() {
    const GrindAbortReason all[] = {GrindAbortReason::NONE,       GrindAbortReason::SENSOR,
                                    GrindAbortReason::TARE,       GrindAbortReason::PRIME_FLOW,
                                    GrindAbortReason::PREDICTIVE_FLOW, GrindAbortReason::STALL,
                                    GrindAbortReason::MOTOR_FAULT, GrindAbortReason::NEG_WEIGHT,
                                    GrindAbortReason::UPDATE};
    for (GrindAbortReason r : all) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)r, (uint8_t)glink_abort_reason_from_grind(r));
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkAbortReason::NONE, (uint8_t)glink_abort_reason_from_grind((GrindAbortReason)200));
}

// Outcome values are GrindController::GrindSessionResult in declaration order
static void test_outcome_maps_to_glink_mirror() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::UNKNOWN, (uint8_t)glink_outcome_from_grind(0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::SUCCESS, (uint8_t)glink_outcome_from_grind(1));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::OVERSHOOT, (uint8_t)glink_outcome_from_grind(2));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::MAX_PULSES, (uint8_t)glink_outcome_from_grind(3));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::TIMEOUT, (uint8_t)glink_outcome_from_grind(4));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::ERROR, (uint8_t)glink_outcome_from_grind(5));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkOutcome::UNKNOWN, (uint8_t)glink_outcome_from_grind(77));
}

static GlinkResultCode code_for_safety_bit(uint8_t bit) {
    return glink_result_code(GlinkOutcome::ERROR,
                             glink_abort_reason_from_grind(grind_abort_reason_from_safety_bit(bit)), false);
}

// Every safety bit, as an ERROR, maps to its GLink result (PRIME_FLOW and PREDICTIVE_FLOW both give ERR_NO_FLOW)
static void test_abort_reason_every_safety_bit_gives_the_matching_result() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::ERR_SENSOR, (uint8_t)code_for_safety_bit(GRIND_SAFETY_REASON_SENSOR));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::ERR_TARE, (uint8_t)code_for_safety_bit(GRIND_SAFETY_REASON_TARE));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::ERR_NO_FLOW, (uint8_t)code_for_safety_bit(GRIND_SAFETY_REASON_PRIME_FLOW));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::ERR_NO_FLOW, (uint8_t)code_for_safety_bit(GRIND_SAFETY_REASON_PREDICTIVE_FLOW));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::ERR_STALLED, (uint8_t)code_for_safety_bit(GRIND_SAFETY_REASON_STALL));
}

// A genuine timeout (stopped = false) stays TIMEOUT; only a real stop request reads as STOPPED
static void test_timeout_stays_timeout_unless_stopped() {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::TIMEOUT,
                            (uint8_t)glink_result_code(glink_outcome_from_grind(4), GlinkAbortReason::NONE, false));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)GlinkResultCode::STOPPED,
                            (uint8_t)glink_result_code(glink_outcome_from_grind(0), GlinkAbortReason::NONE, true));
}

// Source text guard (reads src/controllers/grind_controller.cpp): every ERROR or variable assignment of
// last_session_result_ has an abort_reason_ assignment within the 3 lines before it or the line after
static std::vector<std::string> read_controller_lines() {
    const char* paths[] = {"src/controllers/grind_controller.cpp", "../src/controllers/grind_controller.cpp",
                           "../../src/controllers/grind_controller.cpp"};
    std::vector<std::string> lines;
    for (const char* p : paths) {
        std::ifstream f(p);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) lines.push_back(line);
        break;
    }
    return lines;
}

static void test_every_last_session_result_assignment_has_a_reason() {
    const std::vector<std::string> lines = read_controller_lines();
    TEST_ASSERT_TRUE_MESSAGE(!lines.empty(), "grind_controller.cpp not found");
    int checked = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& l = lines[i];
        const size_t pos = l.find("last_session_result_");
        if (pos == std::string::npos) continue;
        // an assignment is "last_session_result_ =" followed by anything but another '=' (excludes ==)
        const size_t eq = l.find_first_not_of(' ', pos + std::string("last_session_result_").size());
        if (eq == std::string::npos || l[eq] != '=' || (eq + 1 < l.size() && l[eq + 1] == '=')) continue;
        checked++;
        if (l.find("GrindSessionResult::UNKNOWN") != std::string::npos ||
            l.find("GrindSessionResult::TIMEOUT") != std::string::npos) {
            continue;  // reset and timeout carry no reason
        }
        const bool is_error = l.find("GrindSessionResult::ERROR") != std::string::npos;
        const bool is_variable = l.find("= result;") != std::string::npos || l.find("= session_result;") != std::string::npos;
        TEST_ASSERT_TRUE_MESSAGE(is_error || is_variable, l.c_str());
        bool found = false;
        for (size_t k = (i >= 3 ? i - 3 : 0); k <= i + 1 && k < lines.size(); k++) {
            const size_t a = lines[k].find("abort_reason_ = ");
            if (a == std::string::npos) continue;
            // a real reason: not NONE (abort_reason_ == is not matched because of the trailing "= ")
            if (is_error && lines[k].find("GrindAbortReason::NONE", a) != std::string::npos) continue;
            found = true;
        }
        TEST_ASSERT_TRUE_MESSAGE(found, l.c_str());
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(7, checked);
}

static void test_result_may_replace() {
    TEST_ASSERT_TRUE(grind_result_may_replace(false, false, false));
    TEST_ASSERT_TRUE(grind_result_may_replace(false, true, true));   // first result of the session
    TEST_ASSERT_TRUE(grind_result_may_replace(true, true, false));   // a real outcome replaces a raced stop
    TEST_ASSERT_FALSE(grind_result_may_replace(true, false, true));  // a stop never replaces a real outcome
    TEST_ASSERT_FALSE(grind_result_may_replace(true, false, false)); // one real result per session
    TEST_ASSERT_FALSE(grind_result_may_replace(true, true, true));   // second stop
}

void run_grind_safety_tests() {
    RUN_TEST(test_timed_out_normal);
    RUN_TEST(test_timed_out_boundary);
    RUN_TEST(test_timed_out_now_before_start);
    RUN_TEST(test_timed_out_wrap);
    RUN_TEST(test_prime_no_flow_before_check_time);
    RUN_TEST(test_prime_no_flow_at_check_time);
    RUN_TEST(test_prime_no_flow_weight_threshold);
    RUN_TEST(test_prime_no_flow_start_ahead_of_now);
    RUN_TEST(test_prime_no_flow_uses_config_constants);
    RUN_TEST(test_predictive_no_flow_before_check_time);
    RUN_TEST(test_predictive_no_flow_gain_boundary);
    RUN_TEST(test_predictive_no_flow_start_ahead_of_now);
    RUN_TEST(test_predictive_no_flow_config_constants);
    RUN_TEST(test_pulse_wait_limit_normal);
    RUN_TEST(test_pulse_wait_limit_invalid_inputs);
    RUN_TEST(test_pulse_wait_limit_huge_clamped);
    RUN_TEST(test_stall_constants);
    RUN_TEST(test_stall_window_boundary);
    RUN_TEST(test_stall_gain_exact_resets);
    RUN_TEST(test_stall_gain_just_below_does_not_reset);
    RUN_TEST(test_stall_reference_moves_up_only);
    RUN_TEST(test_stall_bump_resets_once);
    RUN_TEST(test_stall_motor_off_rebaselines);
    RUN_TEST(test_stall_motor_off_rebaselines_weight);
    RUN_TEST(test_stall_non_finite_ignored);
    RUN_TEST(test_stall_now_before_last_gain);
    RUN_TEST(test_stall_wrap);
    RUN_TEST(test_stall_window_zero);
    RUN_TEST(test_stall_slow_rise_aborts);
    RUN_TEST(test_stall_fast_rise_does_not_abort);
    RUN_TEST(test_stall_nan_at_reset_then_rising);
    RUN_TEST(test_stall_motor_off_non_finite_then_on);
    RUN_TEST(test_stall_default_initialised);
    RUN_TEST(test_abort_reason_every_safety_bit);
    RUN_TEST(test_abort_reason_safety_bits_are_distinct_single_bits);
    RUN_TEST(test_abort_reason_unknown_or_combined_bits_are_none);
    RUN_TEST(test_abort_reason_maps_to_glink_mirror);
    RUN_TEST(test_outcome_maps_to_glink_mirror);
    RUN_TEST(test_abort_reason_every_safety_bit_gives_the_matching_result);
    RUN_TEST(test_timeout_stays_timeout_unless_stopped);
    RUN_TEST(test_every_last_session_result_assignment_has_a_reason);
    RUN_TEST(test_result_may_replace);
}
