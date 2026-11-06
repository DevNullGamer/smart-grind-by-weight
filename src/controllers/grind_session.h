#pragma once

#include "grind_mode.h"
#include <cstdint>

// Why a grind ended in ERROR (GrindSessionResult::ERROR always carries one besides NONE). Same order as the mirror
// enum GlinkAbortReason (bluetooth/glink_results.h); bluetooth/glink_session_map.* maps one onto the other.
enum class GrindAbortReason : uint8_t {
    NONE = 0,
    SENSOR,           // load cell stale / no sample
    TARE,             // tare did not complete
    PRIME_FLOW,       // no flow while priming
    PREDICTIVE_FLOW,  // no flow in PREDICTIVE, or no weight delivered at the end
    STALL,            // flow stopped mid-grind
    MOTOR_FAULT,      // grinder deadman tripped
    NEG_WEIGHT,       // negative-weight failsafe
    UPDATE            // firmware update (never set by the controller itself)
};

struct GrindSessionDescriptor {
    GrindMode mode = GrindMode::WEIGHT;
    float target_weight = 0.0f;      // grams
    uint32_t target_time_ms = 0;     // milliseconds
    float tolerance = 0.0f;          // grams
    uint8_t profile_id = 0;          // active profile index
};
