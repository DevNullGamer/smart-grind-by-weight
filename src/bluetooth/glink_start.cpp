#include "glink_start.h"

#include <stdio.h>

GlinkLeadInEvent glink_lead_in_step(PortalLeadIn* lead_in, const GlinkLeadInInputs& in, uint32_t now_ms) {
    if (!lead_in->active) return GlinkLeadInEvent::NONE;

    // Why it is cancelled, if this frame does it (the overlay's STOP button cancels from outside: CANCELLED_USER)
    GlinkLeadInEvent reason = GlinkLeadInEvent::NONE;
    if (!in.touch_available) {
        // Nobody could cancel by touch: fail safe, the start is dropped
        reason = GlinkLeadInEvent::CANCELLED_NO_TOUCH;
    } else if (portal_lead_in_touched(*lead_in, now_ms, in.ms_since_last_touch)) {
        reason = GlinkLeadInEvent::CANCELLED_TOUCH;
    } else if (in.stop_pending) {
        reason = GlinkLeadInEvent::CANCELLED_STOP;
    }
    if (reason != GlinkLeadInEvent::NONE) portal_lead_in_cancel(lead_in);

    switch (portal_lead_in_poll(lead_in, now_ms)) {  // clears the lead-in on CANCELLED and GO; CANCELLED wins over GO
        case PortalLeadInState::CANCELLED:
            return reason != GlinkLeadInEvent::NONE ? reason : GlinkLeadInEvent::CANCELLED_USER;
        case PortalLeadInState::GO:
            return GlinkLeadInEvent::GO;
        case PortalLeadInState::WAITING:
            if (!in.link_enabled) {
                *lead_in = PortalLeadIn();
                return GlinkLeadInEvent::DROPPED_LINK_OFF;
            }
            if (!in.start_allowed) {
                *lead_in = PortalLeadIn();
                return GlinkLeadInEvent::DROPPED_STATE;
            }
            return GlinkLeadInEvent::WAITING;
        case PortalLeadInState::IDLE:
            break;
    }
    return GlinkLeadInEvent::NONE;
}

uint32_t glink_lead_in_remaining_ms(const PortalLeadIn& lead_in, uint32_t now_ms) {
    if (!lead_in.active) return 0;
    const uint32_t elapsed = static_cast<uint32_t>(now_ms - lead_in.start_ms);
    return elapsed >= PORTAL_LEAD_IN_MS ? 0 : PORTAL_LEAD_IN_MS - elapsed;
}

void glink_lead_in_text(char* out, size_t cap, const char* profile_name, int32_t dose_tenths, uint32_t remaining_s) {
    if (!out || cap == 0) return;
    const int32_t whole = dose_tenths / 10;
    const int32_t frac = (dose_tenths < 0 ? -dose_tenths : dose_tenths) % 10;
    snprintf(out, cap, "Remote start: %s %d.%d g in %u s, touch to cancel", profile_name ? profile_name : "", static_cast<int>(whole),
             static_cast<int>(frac), static_cast<unsigned>(remaining_s));
}

bool glink_refusal_shows_dialog(GlinkScreen screen, const GlinkStartInputs& inputs, const GlinkFailure& failure) {
    if (screen != GlinkScreen::READY || inputs.block == GlinkStartBlock::NONE) return false;
    const char* message = "";
    const GlinkError block_error = glink_start_block_error(inputs.block, &message);
    // The profile checks come before the block in 7.2: a refusal of one of them shows nothing
    return failure.code != GlinkError::NONE && failure.code == block_error;
}

GlinkOrigin glink_origin_of_session(uint32_t session_id, uint32_t ble_session_id, uint32_t web_session_id) {
    if (session_id != 0 && session_id == ble_session_id) return GlinkOrigin::BLE;
    if (session_id != 0 && session_id == web_session_id) return GlinkOrigin::WEB;
    return GlinkOrigin::TOUCH;
}
