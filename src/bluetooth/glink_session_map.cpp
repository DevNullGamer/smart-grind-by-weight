#include "glink_session_map.h"
#include "glink_screen.h"

GlinkOutcome glink_outcome_from_grind(uint8_t session_result) {
    if (session_result > static_cast<uint8_t>(GlinkOutcome::ERROR)) return GlinkOutcome::UNKNOWN;
    return static_cast<GlinkOutcome>(session_result);
}

GlinkAbortReason glink_abort_reason_from_grind(GrindAbortReason reason) {
    if (static_cast<uint8_t>(reason) > static_cast<uint8_t>(GlinkAbortReason::UPDATE)) return GlinkAbortReason::NONE;
    return static_cast<GlinkAbortReason>(static_cast<uint8_t>(reason));
}

GlinkResultInfo glink_result_from_record(const GlinkGrindRecord& record) {
    GlinkResultInfo info;
    info.seq = record.seq;
    info.code = glink_result_code(glink_outcome_from_grind(record.outcome),
                                  glink_abort_reason_from_grind(record.abort_reason), record.stopped);
    info.origin = record.origin;
    info.profile = record.profile_id;
    info.mode = record.time_mode ? GlinkMode::TIME : GlinkMode::WEIGHT;
    info.target_tenths = glink_tenths_from_float(record.time_mode ? static_cast<float>(record.target_time_ms) / 1000.0f
                                                                  : record.target_weight_g);
    info.final_weight_tenths = glink_tenths_from_float(record.final_weight_g);
    info.weight_settled = glink_result_weight_settled(info.code);
    info.pulses = record.pulses;
    info.duration_ms = record.total_time_ms;
    info.motor_ms = record.motor_time_ms;
    return info;
}
