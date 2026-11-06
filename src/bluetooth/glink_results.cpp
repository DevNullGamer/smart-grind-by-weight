#include "glink_results.h"

GlinkResultCode glink_result_code(GlinkOutcome outcome, GlinkAbortReason reason, bool stopped) {
    if (stopped && (outcome == GlinkOutcome::UNKNOWN || outcome == GlinkOutcome::TIMEOUT)) {
        return GlinkResultCode::STOPPED;
    }
    switch (outcome) {
        case GlinkOutcome::SUCCESS: return GlinkResultCode::OK;
        case GlinkOutcome::OVERSHOOT: return GlinkResultCode::OVERSHOOT;
        case GlinkOutcome::MAX_PULSES: return GlinkResultCode::MAX_PULSES;
        case GlinkOutcome::TIMEOUT:
        case GlinkOutcome::UNKNOWN: return GlinkResultCode::TIMEOUT;
        case GlinkOutcome::ERROR: break;
    }
    switch (reason) {
        case GlinkAbortReason::STALL: return GlinkResultCode::ERR_STALLED;
        case GlinkAbortReason::PRIME_FLOW:
        case GlinkAbortReason::PREDICTIVE_FLOW: return GlinkResultCode::ERR_NO_FLOW;
        case GlinkAbortReason::TARE: return GlinkResultCode::ERR_TARE;
        case GlinkAbortReason::MOTOR_FAULT: return GlinkResultCode::ERR_MOTOR;
        case GlinkAbortReason::NEG_WEIGHT: return GlinkResultCode::ERR_NEG_WEIGHT;
        case GlinkAbortReason::UPDATE: return GlinkResultCode::UPDATE;
        case GlinkAbortReason::SENSOR:
        case GlinkAbortReason::NONE: return GlinkResultCode::ERR_SENSOR;
    }
    return GlinkResultCode::ERR_SENSOR;
}

bool glink_result_weight_settled(GlinkResultCode code) {
    return code == GlinkResultCode::OK || code == GlinkResultCode::OVERSHOOT || code == GlinkResultCode::MAX_PULSES;
}
