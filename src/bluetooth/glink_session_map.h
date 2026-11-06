#pragma once
#include <stdint.h>
#include "../controllers/grind_session.h"
#include "glink_results.h"

// Maps the grind controller's records onto the GLink mirror enums of glink_results.h. Pure (host-tested).
// The outcome comes in as the underlying value of GrindController::GrindSessionResult (a nested enum that cannot be
// included on the host); grind_controller.cpp static_asserts that the order matches GlinkOutcome.
GlinkOutcome glink_outcome_from_grind(uint8_t session_result);

// Unknown values map to NONE.
GlinkAbortReason glink_abort_reason_from_grind(GrindAbortReason reason);

// One finished grind as the UI task reads it from the seam (GrindController::GrindResult plus the session descriptor
// and the origin), flattened so it needs no controller header. `outcome` is the underlying value of GrindSessionResult.
struct GlinkGrindRecord {
    uint32_t seq = 0;                // GrindController::result_count() read with the result (the periodInfo resultSeq)
    uint8_t outcome = 0;
    GrindAbortReason abort_reason = GrindAbortReason::NONE;
    bool stopped = false;
    float final_weight_g = 0.0f;
    uint8_t pulses = 0;
    uint32_t total_time_ms = 0;
    uint32_t motor_time_ms = 0;
    uint8_t profile_id = 0;          // 0..3
    bool time_mode = false;
    float target_weight_g = 0.0f;
    uint32_t target_time_ms = 0;
    GlinkOrigin origin = GlinkOrigin::TOUCH;
};

// The wire result (docs/GLINK-PROTOCOL.md 5.6): code from outcome / abort reason / stop (glink_result_code), settled
// flag from the code, weights and the target rounded half away from zero to tenths (seconds in TIME mode).
GlinkResultInfo glink_result_from_record(const GlinkGrindRecord& record);
