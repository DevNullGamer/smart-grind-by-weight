#include "glink_screen.h"
#include <math.h>

GlinkScreen glink_classify_screen(const GlinkStateInputs& in) {
    if (in.lead_in_active) return GlinkScreen::LEAD_IN;
    if (in.update_active || in.ui_state == UIState::OTA_UPDATE) return GlinkScreen::UPDATING;
    switch (in.ui_state) {
        case UIState::GRINDING:
        case UIState::PURGE_CONFIRM: return GlinkScreen::GRINDING;
        case UIState::GRIND_COMPLETE:
        case UIState::GRIND_TIMEOUT: return GlinkScreen::DONE;
        case UIState::EDIT: return GlinkScreen::EDIT;
        case UIState::READY: return (in.overlay_active || in.busy) ? GlinkScreen::OTHER : GlinkScreen::READY;
        case UIState::MENU: return (in.overlay_active || in.busy) ? GlinkScreen::OTHER : GlinkScreen::MENU;
        default: break;  // calibration, confirm dialogs, autotune, failed update
    }
    return GlinkScreen::OTHER;
}

GlinkStartBlock glink_start_block_from_grind(GrindStartBlock block) {
    switch (block) {
        case GrindStartBlock::NONE: return GlinkStartBlock::NONE;
        case GrindStartBlock::MOTOR_DISABLED: return GlinkStartBlock::MOTOR_DISABLED;
        case GrindStartBlock::UPDATE_IN_PROGRESS: return GlinkStartBlock::UPDATE_LOCK;
        case GrindStartBlock::LOAD_CELL_NOT_CONNECTED: return GlinkStartBlock::LOAD_CELL_NOT_CONNECTED;
        case GrindStartBlock::LOAD_CELL_NO_DATA: return GlinkStartBlock::LOAD_CELL_NO_DATA;
        case GrindStartBlock::LOAD_CELL_RATE_INVALID: return GlinkStartBlock::LOAD_CELL_RATE_INVALID;
    }
    return GlinkStartBlock::NONE;
}

// GrindPhase: IDLE 0, INITIALIZING 1 .. TIME_ADDITIONAL_PULSE 11 (same order as GlinkPhase), COMPLETED 12, TIMEOUT 13,
// PRIME 14, PRIME_SETTLING 15, PURGE_CONFIRM 16
GlinkPhase glink_phase_from_grind(int grind_phase_id) {
    if (grind_phase_id >= 1 && grind_phase_id <= 11) return static_cast<GlinkPhase>(grind_phase_id);
    if (grind_phase_id >= 14 && grind_phase_id <= 16) return static_cast<GlinkPhase>(grind_phase_id - 2);
    return GlinkPhase::NONE;
}

int32_t glink_tenths_from_float(float value) {
    if (!isfinite(value)) return 0;
    const float scaled = value * 10.0f;
    if (scaled >= 2147483000.0f) return 2147483000;
    if (scaled <= -2147483000.0f) return -2147483000;
    return static_cast<int32_t>(lroundf(scaled));
}

GlinkPeriodInfo glink_build_period(const GlinkPeriodInputs& in) {
    GlinkPeriodInfo info;
    info.dev_state = glink_dev_state(in.screen, in.block);
    info.weight_valid = in.weight_valid && isfinite(in.weight_g);
    info.weight_tenths = info.weight_valid ? glink_tenths_from_float(in.weight_g) : 0;
    info.select_profile = in.select_profile;
    if (info.dev_state == GlinkDevState::GRINDING) {
        info.phase = glink_phase_from_grind(in.grind_phase_id);
        info.has_target = true;
        info.target_tenths = glink_tenths_from_float(in.target);
        info.mode = in.time_mode ? GlinkMode::TIME : GlinkMode::WEIGHT;
    }
    info.blocked = glink_blocked(info.dev_state, in.block);
    info.result_seq = in.result_seq;
    info.profile_rev = in.profile_rev;
    return info;
}
