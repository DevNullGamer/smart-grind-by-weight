#pragma once
#include <stdint.h>
#include "glink_messages.h"

// Mapping of a finished grind session onto the GLink result code (docs/GLINK-PROTOCOL.md 6.3). Pure. The enums
// below mirror GrindController::GrindSessionResult and the abort reasons of the grind controller, kept here so this
// module has no controller dependency; the glue code that publishes results maps the controller's values onto them
// (step 3).
enum class GlinkOutcome : uint8_t { UNKNOWN = 0, SUCCESS, OVERSHOOT, MAX_PULSES, TIMEOUT, ERROR };

enum class GlinkAbortReason : uint8_t {
    NONE = 0,
    SENSOR,
    TARE,
    PRIME_FLOW,
    PREDICTIVE_FLOW,
    STALL,
    MOTOR_FAULT,
    NEG_WEIGHT,
    UPDATE
};

// outcome / reason: what the controller recorded; stopped: the session ended by a stop request (touch, web or BLE).
// A stop explains the end only when the grind has no outcome of its own (UNKNOWN, or the TIMEOUT a stopped grind
// falls into): a normal completion and a real error are reported as they are. A grind ending in ERROR without a
// known reason (NONE) is ERR_SENSOR.
GlinkResultCode glink_result_code(GlinkOutcome outcome, GlinkAbortReason reason, bool stopped);

// True for the codes whose final weight is the settled weight (OK, OVERSHOOT, MAX_PULSES).
bool glink_result_weight_settled(GlinkResultCode code);
