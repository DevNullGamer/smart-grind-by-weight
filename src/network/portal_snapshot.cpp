#include "network/portal_snapshot.h"
#include <math.h>
#include <string.h>

namespace {

// Indexed by the numeric value of GrindPhase (same order as the enum)
const char* const kGrindPhaseNames[PORTAL_GRIND_PHASE_COUNT] = {
    "idle",            "initializing",  "setup",          "taring",
    "tare_confirm",    "predictive",    "pulse_decision", "pulse_execute",
    "pulse_settling",  "final_settling", "time_grinding", "time_additional_pulse",
    "completed",       "timeout",       "prime",          "prime_settling",
    "purge_confirm",
};

}  // namespace

const char* portal_ui_state_name(UIState state) {
    switch (state) {
        case UIState::READY: return "ready";
        case UIState::GRINDING: return "grinding";
        case UIState::GRIND_COMPLETE: return "grind_complete";
        case UIState::GRIND_TIMEOUT: return "grind_timeout";
        case UIState::EDIT: return "edit";
        case UIState::MENU: return "menu";
        case UIState::CALIBRATION: return "calibration";
        case UIState::CONFIRM: return "confirm";
        case UIState::PURGE_CONFIRM: return "purge_confirm";
        case UIState::AUTOTUNING: return "autotuning";
        case UIState::OTA_UPDATE: return "ota_update";
        case UIState::OTA_UPDATE_FAILED: return "ota_update_failed";
    }
    return "unknown";
}

const char* portal_grind_phase_name(int phase_id) {
    if (phase_id < 0 || static_cast<size_t>(phase_id) >= PORTAL_GRIND_PHASE_COUNT) return "unknown";
    return kGrindPhaseNames[phase_id];
}

PortalUiMode portal_ui_mode(UIState state, bool overlay_active) {
    if (overlay_active) return PortalUiMode::OTHER;
    if (state == UIState::READY) return PortalUiMode::READY;
    if (state == UIState::MENU) return PortalUiMode::MENU;
    return PortalUiMode::OTHER;
}

PortalStatus portal_build_status(const PortalStatusInputs& in) {
    PortalStatus status;
    status.ui_state = portal_ui_state_name(in.ui_state);
    status.grind_phase = portal_grind_phase_name(in.grind_phase_id);
    status.busy = in.busy;
    status.weight_g = (in.weight_valid && isfinite(in.weight_g)) ? in.weight_g : NAN;
    status.motor_available = in.motor_available;
    status.start_block = portal_error_from_start_block(in.start_block);
    status.motor_test_block = portal_error_from_motor_test_block(in.motor_test_block);
    status.uptime_s = in.uptime_s;
    status.build = in.build;
    status.web_interface = true;
    return status;
}

void portal_device_name_from_host(const char* host, char* out) {
    out[0] = '\0';
    if (!host) return;
    size_t n = strnlen(host, PORTAL_DEVICE_NAME_MAX_LEN + 1);
    if (n > PORTAL_DEVICE_NAME_MAX_LEN) {
        n = PORTAL_DEVICE_NAME_MAX_LEN;
        while (n > 1 && host[n - 1] == '-') n--;
    }
    memcpy(out, host, n);
    out[n] = '\0';
}
