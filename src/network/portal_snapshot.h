#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"
#include "../controllers/grind_start_check.h"
#include "../controllers/motor_test_check.h"
#include "../system/state_machine.h"  // UIState (the enum only)
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
};

PortalStatus portal_build_status(const PortalStatusInputs& in);

// The device name as the API reports it, from the full mDNS host name (up to 63 characters): a name that fits
// PORTAL_DEVICE_NAME_MAX_LEN is copied as is, a longer one (only possible if it was written outside this API)
// is cut to that length with trailing hyphens removed, so the reported name always passes the validator.
// out must hold PORTAL_DEVICE_NAME_MAX_LEN + 1 bytes.
void portal_device_name_from_host(const char* host, char* out);
