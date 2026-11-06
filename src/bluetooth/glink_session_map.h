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
