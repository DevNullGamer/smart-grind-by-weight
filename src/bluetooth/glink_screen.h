#pragma once
#include <stdint.h>
#include "../controllers/grind_start_check.h"
#include "../system/state_machine.h"
#include "glink_decide.h"
#include "glink_messages.h"

// From the grinder's real state to the GLink vocabulary (docs/GLINK-PROTOCOL.md 6.1, 6.2, 7.1): the screen class of the
// write table, the grind phase, the start block and the periodInfo fields. Pure (no LVGL, no Arduino): the UI task
// gathers the inputs, these functions decide. Host-tested.

struct GlinkStateInputs {
    UIState ui_state = UIState::READY;
    bool overlay_active = false;    // a blocking overlay or dialog is up
    bool busy = false;              // a grind, autotune or motor output is active
    bool update_active = false;     // an update is held, running or being offered
    bool lead_in_active = false;    // a remote start (web panel) is in its lead-in
};

// The rows of the table in 7.1, evaluated from the top: LEAD_IN, UPDATING, then by screen. READY and MENU only count
// without an overlay and while nothing is busy; the grinding screens are GRINDING whatever is on top of them.
GlinkScreen glink_classify_screen(const GlinkStateInputs& inputs);

GlinkStartBlock glink_start_block_from_grind(GrindStartBlock block);

// GrindPhase (numeric value, controllers/grind_controller.h) to the wire phase. IDLE, COMPLETED, TIMEOUT and out-of-range
// values give NONE (absent in periodInfo). The firmware static_asserts the numbers it relies on.
GlinkPhase glink_phase_from_grind(int grind_phase_id);

struct GlinkPeriodInputs {
    GlinkScreen screen = GlinkScreen::OTHER;
    GlinkStartBlock block = GlinkStartBlock::NONE;
    int grind_phase_id = 0;
    bool weight_valid = false;
    float weight_g = 0.0f;
    uint8_t select_profile = 0;
    bool time_mode = false;         // the running grind is a time grind
    float target = 0.0f;            // g (weight mode) or s (time mode) of the running grind
    uint32_t result_seq = 0;
    uint32_t profile_rev = 0;
};

// Weight and target are rounded half away from zero to tenths; a non-finite weight is "invalid". `phase` and `target`
// are present only while GRINDING.
GlinkPeriodInfo glink_build_period(const GlinkPeriodInputs& inputs);

// Value rounded half away from zero to tenths (the wire resolution of every number)
int32_t glink_tenths_from_float(float value);
