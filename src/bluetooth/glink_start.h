#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../controllers/grind_mode.h"
#include "../network/portal_guard.h"
#include "glink_decide.h"
#include "glink_messages.h"

// The pure rules of a GLink start (docs/GLINK-PROTOCOL.md 5.5, plan chg020 4.6): the touch-to-cancel lead-in step, the
// text of its overlay, when a refusal at GO opens the start-block dialog, which origin a result carries and the
// restoration of the user's grind mode after a forced weight-mode session. The UI task (ui/controllers/
// glink_controller.*) gathers the real state and calls these; time is injected and wrap-safe. No Arduino/LVGL/NVS, host
// tested in test_native/test_glink_start.cpp.

static_assert(GLINK_START_LEAD_IN_MS == PORTAL_LEAD_IN_MS,
              "the BLE lead-in reuses portal_lead_in_*: both must keep the same length");

//------------------------------------------------------------------------------
// Lead-in
//------------------------------------------------------------------------------
enum class GlinkLeadInEvent : uint8_t {
    NONE,                  // no lead-in running
    WAITING,               // counting down
    GO,                    // time is up: decide again and start (reported once; the lead-in is cleared)
    CANCELLED_TOUCH,       // a touch after the lead-in began
    CANCELLED_NO_TOUCH,    // no touch driver: nobody could cancel, so the start is dropped
    CANCELLED_STOP,        // a stop request (BLE, web or touch STOP routed through the counter)
    CANCELLED_USER,        // the overlay's own STOP button
    DROPPED_LINK_OFF,      // the GaggiMate link / Bluetooth was switched off during the lead-in
    DROPPED_STATE          // the grinder state no longer allows a start (update, fault, another screen, ...)
};

struct GlinkLeadInInputs {
    bool touch_available = false;        // a touch driver exists to report the last touch
    uint32_t ms_since_last_touch = 0;    // the touch driver's age of its last touch
    bool stop_pending = false;           // a stop request arrived since it was last handled
    bool link_enabled = true;            // GLink is still served (Bluetooth / link switch on)
    bool start_allowed = true;           // the full start decision still passes (own overlay left out of the screen state)
};

// One frame of the lead-in. A cancellation of any kind wins over GO in the same frame, so a touch, STOP or `stop` that
// arrives exactly when the time is up still cancels. The lead-in is cleared on every result other than NONE/WAITING.
GlinkLeadInEvent glink_lead_in_step(PortalLeadIn* lead_in, const GlinkLeadInInputs& inputs, uint32_t now_ms);

// Milliseconds until the lead-in ends (0 when none runs or it is due)
uint32_t glink_lead_in_remaining_ms(const PortalLeadIn& lead_in, uint32_t now_ms);

// A stop with nothing to cancel is forgotten (never queued) - unless a request is queued that may begin a lead-in this
// very frame: that lead-in must see the stop that arrived before it.
inline bool glink_consume_idle_stop(bool lead_in_active, bool request_queued) { return !lead_in_active && !request_queued; }

// "Remote start: PROFILE 18.0 g in 3 s, touch to cancel" (dose in tenths of a gram)
void glink_lead_in_text(char* out, size_t cap, const char* profile_name, int32_t dose_tenths, uint32_t remaining_s);

// 5.5: when the decision at GO fails because of a start block, the grinder opens its confirmation dialog for it; every
// other refusal shows nothing. `failure` is what glink_decide_start returned for `inputs` on `screen`.
bool glink_refusal_shows_dialog(GlinkScreen screen, const GlinkStartInputs& inputs, const GlinkFailure& failure);

//------------------------------------------------------------------------------
// Who started the grind
//------------------------------------------------------------------------------
// A result carries BLE or WEB when its session id is the one a GLink / web start recorded, else TOUCH (button, auto
// start). 0 = no such start (session ids are 1-based).
GlinkOrigin glink_origin_of_session(uint32_t session_id, uint32_t ble_session_id, uint32_t web_session_id);

//------------------------------------------------------------------------------
// Forced weight mode
//------------------------------------------------------------------------------
// A GLink start grinds in WEIGHT mode whatever the grinder's grind mode is. arm() when such a session starts; on_ready()
// when the screen returns to READY says whether the user's stored mode must be put back (UIManager::current_mode and
// the grinding screen). Only an armed guard restores, so touch and web grinds are untouched.
class GlinkForcedMode {
public:
    void arm() { armed_ = true; }
    bool armed() const { return armed_; }
    // true (once) when armed; *restore_to is the stored mode of the user
    bool on_ready(GrindMode stored_mode, GrindMode* restore_to) {
        if (!armed_) return false;
        armed_ = false;
        *restore_to = stored_mode;
        return true;
    }

private:
    bool armed_ = false;
};
