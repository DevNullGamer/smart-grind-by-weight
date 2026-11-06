#include "glink_session_map.h"

GlinkOutcome glink_outcome_from_grind(uint8_t session_result) {
    if (session_result > static_cast<uint8_t>(GlinkOutcome::ERROR)) return GlinkOutcome::UNKNOWN;
    return static_cast<GlinkOutcome>(session_result);
}

GlinkAbortReason glink_abort_reason_from_grind(GrindAbortReason reason) {
    if (static_cast<uint8_t>(reason) > static_cast<uint8_t>(GlinkAbortReason::UPDATE)) return GlinkAbortReason::NONE;
    return static_cast<GlinkAbortReason>(static_cast<uint8_t>(reason));
}
