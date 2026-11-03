#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"
#include "../controllers/grind_start_check.h"
#include "../controllers/motor_test_check.h"
#include "../system/state_machine.h"  // UIState (the enum only)
#include "../system/statistics_snapshot.h"
#include "portal_api.h"
#include "portal_guard.h"

// Pure helpers behind the UI-task snapshot of the web panel (no Arduino/ESP dependencies, unit tested on the
// host): the names a status reply uses, how the screen state maps to what a write may do, and the conversion
// of the inputs the UI task gathers into a PortalStatus.

// Lower-case snake_case name of a screen state ("ready", "grind_complete", ...)
const char* portal_ui_state_name(UIState state);

// Number of GrindPhase values (controllers/grind_controller.h); the firmware asserts it against the enum
constexpr size_t PORTAL_GRIND_PHASE_COUNT = 17;
// Lower-case name of a GrindPhase by its numeric value ("idle", "predictive", "time_grinding", ...);
// "unknown" for anything else
const char* portal_grind_phase_name(int phase_id);

// What the screen allows right now: READY and MENU only count when no blocking overlay is up; every other
// state (grinding screens, EDIT, CALIBRATION, CONFIRM dialogs, updates, ...) is OTHER
PortalUiMode portal_ui_mode(UIState state, bool overlay_active);

struct PortalStatusInputs {
    UIState ui_state = UIState::READY;
    int grind_phase_id = 0;
    bool busy = false;
    bool weight_valid = false;  // the load cell delivers a reading
    float weight_g = 0.0f;
    bool motor_available = false;
    GrindStartBlock start_block = GrindStartBlock::NONE;
    MotorTestBlock motor_test_block = MotorTestBlock::NONE;
    uint32_t uptime_s = 0;
    int32_t build = -1;
    uint32_t diag_mask = 0;  // active DiagnosticCode values, bit n = code n (DiagnosticsController::active_mask)
};

// DiagnosticCode (numeric value, system/diagnostics_controller.h) behind warning index 0 .. PORTAL_WARNING_COUNT - 1,
// in the priority order of portal_api.cpp kWarningNames. The firmware asserts these against the enum
// (portal_ui_controller.cpp); the contract test checks the order against get_highest_priority_warning.
constexpr int PORTAL_WARNING_DIAG_CODES[PORTAL_WARNING_COUNT] = {
    6,  // BOARD_UNRECOGNIZED
    8,  // MOTOR_FAULT
    7,  // MOTOR_DISABLED
    1,  // HX711_NOT_CONNECTED
    9,  // HX711_NO_DATA
    2,  // HX711_SAMPLE_RATE_INVALID
    5,  // MECHANICAL_INSTABILITY
    4,  // LOAD_CELL_NOISY_SUSTAINED
    3,  // LOAD_CELL_NOT_CALIBRATED
};
// Status warnings bitmask (PortalStatus::warnings) of a diagnostics mask: unknown bits, including NONE (bit 0), drop
uint16_t portal_warnings_from_diag_mask(uint32_t diag_mask);

PortalStatus portal_build_status(const PortalStatusInputs& in);

// The statistics reply values of a copy of the raw counters (StatisticsManager::get_snapshot): motor run time in whole
// seconds (sec * 1000 + ms remainder, divided by 1000, saturating at UINT32_MAX), powered minutes (hours * 60 +
// remainder), the two averages as sum / sample count (0 with no samples). The weight buckets keep their meaning
// (single <= 10 g, double <= 22 g, custom larger).
PortalStats portal_stats_from_counters(const StatisticsSnapshot& raw);

// The device name as the API reports it, from the full mDNS host name (up to 63 characters): a name that fits
// PORTAL_DEVICE_NAME_MAX_LEN is copied as is, a longer one (only possible if it was written outside this API)
// is cut to that length with trailing hyphens removed, so the reported name always passes the validator.
// out must hold PORTAL_DEVICE_NAME_MAX_LEN + 1 bytes.
void portal_device_name_from_host(const char* host, char* out);
