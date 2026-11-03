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

uint16_t portal_warnings_from_diag_mask(uint32_t diag_mask) {
    uint16_t warnings = 0;
    for (size_t i = 0; i < PORTAL_WARNING_COUNT; i++) {
        if ((diag_mask >> PORTAL_WARNING_DIAG_CODES[i]) & 1u) warnings = static_cast<uint16_t>(warnings | (1u << i));
    }
    return warnings;
}

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
    status.warnings = portal_warnings_from_diag_mask(in.diag_mask);
    return status;
}

PortalStats portal_stats_from_counters(const StatisticsSnapshot& raw) {
    PortalStats s;
    s.total_grinds = raw.total_grinds;
    s.single = raw.single_shots;
    s.double_shots = raw.double_shots;
    s.custom = raw.custom_shots;
    s.weight_mode = raw.weight_mode_grinds;
    s.time_mode = raw.time_mode_grinds;
    s.total_weight_kg = raw.total_weight_kg;
    const uint64_t motor_ms = static_cast<uint64_t>(raw.motor_runtime_sec) * 1000ULL + raw.motor_runtime_ms_remainder;
    const uint64_t motor_s = motor_ms / 1000ULL;
    s.motor_runtime_s = motor_s > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(motor_s);
    s.avg_abs_error_g = raw.accuracy_sample_count == 0 ? 0.0f : raw.accuracy_sum / static_cast<float>(raw.accuracy_sample_count);
    s.total_pulses = raw.total_pulses;
    s.avg_pulses = raw.pulse_sample_count == 0 ? 0.0f : raw.pulse_sum / static_cast<float>(raw.pulse_sample_count);
    s.time_pulses = raw.time_pulses;
    s.powered_minutes = static_cast<uint64_t>(raw.device_uptime_hrs) * 60ULL + raw.device_uptime_min_remainder;
    return s;
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
