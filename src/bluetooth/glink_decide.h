#pragma once
#include <stddef.h>
#include <stdint.h>
#include "glink_messages.h"

// GLink write / start decisions (docs/GLINK-PROTOCOL.md sections 4.1 steps 5 and 6, 6.5, 7). Pure state tables and
// rules: the UI task gathers the real state (screen, profile 3, start block, clock) and calls these; they decide
// and produce the exact error (code, message, field). No Arduino/ESP dependencies, unit tested on the host.

// The state classes of the table in 7.1.
enum class GlinkScreen : uint8_t {
    LEAD_IN,   // a remote start lead-in is running
    UPDATING,  // any firmware update or its prompt
    READY,     // READY screen without a dialog
    MENU,
    EDIT,
    GRINDING,  // a grind, autotune or motor run in progress
    DONE,      // grind-complete and timeout screens
    OTHER,     // calibration, dialogs and every other screen
    COUNT
};

enum class GlinkOpClass : uint8_t { GET, UPDATE, SELECT, START };

// The grinder's own start block (7.2 items 7 to 11), in the order they are checked.
enum class GlinkStartBlock : uint8_t {
    NONE,
    MOTOR_DISABLED,
    UPDATE_LOCK,
    LOAD_CELL_NOT_CONNECTED,
    LOAD_CELL_NO_DATA,
    LOAD_CELL_RATE_INVALID
};

// 7.1: refusal of an operation class in a screen state (message included). true = accept (for START this means
// "continue with the decide chain"). `object` is the GlinkObject the failure is answered under.
bool glink_state_check(GlinkScreen screen, GlinkOpClass op, int object, GlinkFailure& failure);

// 4.1 steps 5 and 6: INVALID_STATE before the handshake (appHello and a stop pass), BUSY while a request is pending.
// true = pass on. Call after glink_parse_request succeeded.
bool glink_gate(bool handshaken, bool request_pending, const GlinkRequest& request, GlinkFailure& failure);

// 7.3: may a stored change be made now? have_last false = none since boot. Wrap-safe millisecond clock.
bool glink_write_interval_ok(bool have_last, uint32_t last_store_ms, uint32_t now_ms);

//------------------------------------------------------------------------------
// grindProfile update (5.4) and select
//------------------------------------------------------------------------------
struct GlinkProfileState {
    bool exists = false;  // profile 3 only: false until a dose has been set
    int32_t dose_tenths = 0;
    bool has_grind_size = false;
    int32_t grind_size_tenths = 0;
    char grind_unit[GLINK_UNIT_BUF] = {};
};

struct GlinkUpdatePlan {
    GlinkProfileEntry result;  // the resulting entry of profile 3
    bool changed = false;      // a value differs from before; the caller stores only when true
};

// Order after the BLE-task validation: state rules (7.1), create-needs-dose, unit-needs-size, RATE_LIMITED. `request`
// is a valid update of profile 3. The plan holds the resulting entry; `changed` false means success without a write.
// STORE_FAILED is the caller's (the flash write is the last step).
bool glink_decide_update(GlinkScreen screen, const GlinkProfileState& current, const GlinkRequest& request,
                         bool write_interval_ok, GlinkUpdatePlan& plan, GlinkFailure& failure);

// select: state rules, PROFILE_NOT_SET for index 3 that does not exist, RATE_LIMITED for a change. *will_store is
// true when the selection differs from `current_selection` (selecting the selected profile writes nothing).
bool glink_decide_select(GlinkScreen screen, uint8_t index, bool profile3_exists, uint8_t current_selection,
                         bool write_interval_ok, bool* will_store, GlinkFailure& failure);

//------------------------------------------------------------------------------
// grindControl start (7.2)
//------------------------------------------------------------------------------
struct GlinkStartInputs {
    bool profile_exists = true;  // false only for profile 3 before a dose was set
    int32_t stored_dose_tenths = 0;
    bool has_expect_dose = false;
    int32_t expect_dose_tenths = 0;
    GlinkStartBlock block = GlinkStartBlock::NONE;
};

// The whole order of 7.2: COMMAND_IN_FLIGHT, UPDATING, NOT_READY, PROFILE_NOT_SET, OUT_OF_RANGE (stored dose),
// CONFLICT, then the grinder's start block. true = accepted (the lead-in may begin).
bool glink_decide_start(GlinkScreen screen, const GlinkStartInputs& inputs, GlinkFailure& failure);

// The error (and message) a start block stands for. NONE gives GlinkError::NONE.
GlinkError glink_start_block_error(GlinkStartBlock block, const char** message);

//------------------------------------------------------------------------------
// devState and blocked (6.1, 6.5)
//------------------------------------------------------------------------------
GlinkDevState glink_dev_state(GlinkScreen screen, GlinkStartBlock block);

// The `blocked` value of periodInfo: GlinkError::NONE = null.
GlinkError glink_blocked(GlinkDevState state, GlinkStartBlock block);
