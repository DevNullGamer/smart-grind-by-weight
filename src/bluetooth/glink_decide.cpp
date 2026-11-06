#include "glink_decide.h"
#include <string.h>

namespace {

struct Refusal {
    GlinkError code;
    const char* message;
};

constexpr Refusal ACCEPT = {GlinkError::NONE, ""};

// The table of 7.1, one row per screen state and one column per operation class.
Refusal state_refusal(GlinkScreen screen, GlinkOpClass op) {
    switch (screen) {
        case GlinkScreen::LEAD_IN:
            if (op == GlinkOpClass::GET) return ACCEPT;
            return {GlinkError::COMMAND_IN_FLIGHT, glink_msg::IN_FLIGHT};
        case GlinkScreen::UPDATING: return {GlinkError::UPDATING, glink_msg::UPDATING};
        case GlinkScreen::READY: return ACCEPT;  // start continues with the decide chain
        case GlinkScreen::MENU:
            if (op == GlinkOpClass::GET || op == GlinkOpClass::UPDATE) return ACCEPT;
            return {GlinkError::NOT_READY, glink_msg::NOT_READY_READY};
        case GlinkScreen::EDIT:
            if (op == GlinkOpClass::GET) return ACCEPT;
            if (op == GlinkOpClass::UPDATE) return {GlinkError::EDIT_IN_PROGRESS, glink_msg::EDIT_IN_PROGRESS};
            return {GlinkError::NOT_READY, glink_msg::NOT_READY_READY};
        case GlinkScreen::GRINDING:
        case GlinkScreen::DONE:
        case GlinkScreen::OTHER:
            if (op == GlinkOpClass::GET) return ACCEPT;
            return {GlinkError::NOT_READY,
                    op == GlinkOpClass::UPDATE ? glink_msg::NOT_READY_READY_OR_MENU : glink_msg::NOT_READY_READY};
        case GlinkScreen::COUNT: break;
    }
    return {GlinkError::NOT_READY, glink_msg::NOT_READY_READY};
}

// Copies a unit (at most GLINK_GRIND_UNIT_MAX_LEN characters) and terminates it.
void copy_unit(char* dst, const char* src) {
    size_t n = 0;
    while (src[n] != '\0' && n < GLINK_GRIND_UNIT_MAX_LEN) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

bool refuse(GlinkFailure& f, const Refusal& r, const char* field, int object) {
    return glink_set_failure(f, r.code, r.message, field, object);
}

}  // namespace

bool glink_state_check(GlinkScreen screen, GlinkOpClass op, int object, GlinkFailure& failure) {
    const Refusal r = state_refusal(screen, op);
    if (r.code == GlinkError::NONE) return true;
    return refuse(failure, r, "", object);
}

bool glink_gate(bool handshaken, bool request_pending, const GlinkRequest& request, GlinkFailure& failure) {
    const int object = static_cast<int>(request.object);
    if (request.object == GlinkObject::GRIND_CONTROL && request.op == GlinkOp::STOP) return true;
    if (!handshaken && request.object != GlinkObject::APP_HELLO) {
        return glink_set_failure(failure, GlinkError::INVALID_STATE, glink_msg::HANDSHAKE_REQUIRED, "", object);
    }
    if (request_pending) return glink_set_failure(failure, GlinkError::BUSY, glink_msg::BUSY, "", object);
    return true;
}

bool glink_write_interval_ok(bool have_last, uint32_t last_store_ms, uint32_t now_ms) {
    return !have_last || static_cast<uint32_t>(now_ms - last_store_ms) >= GLINK_WRITE_MIN_INTERVAL_MS;
}

bool glink_decide_update(GlinkScreen screen, const GlinkProfileState& current, const GlinkRequest& request,
                         bool write_interval_ok, GlinkUpdatePlan& plan, GlinkFailure& failure) {
    const int object = static_cast<int>(GlinkObject::GRIND_PROFILE);
    if (!glink_state_check(screen, GlinkOpClass::UPDATE, object, failure)) return false;

    if (!current.exists && !request.has_dose) {
        return glink_set_failure(failure, GlinkError::MISSING_FIELD, glink_msg::DOSE_REQUIRED, "dose", object);
    }

    // The resulting values: absent fields stay; grindSize null clears the size and the unit together.
    GlinkProfileEntry r;
    r.index = GLINK_PROFILE_WRITABLE_INDEX;
    r.dose_tenths = request.has_dose ? request.dose_tenths : current.dose_tenths;
    r.has_grind_size = current.exists && current.has_grind_size;
    r.grind_size_tenths = r.has_grind_size ? current.grind_size_tenths : 0;
    if (current.exists) copy_unit(r.grind_unit, current.grind_unit);
    if (request.has_grind_size) {
        r.has_grind_size = !request.grind_size_null;
        r.grind_size_tenths = request.grind_size_null ? 0 : request.grind_size_tenths;
        if (request.grind_size_null) r.grind_unit[0] = '\0';
    }
    if (request.has_grind_unit) copy_unit(r.grind_unit, request.grind_unit);

    if (r.grind_unit[0] != '\0' && !r.has_grind_size) {
        return glink_set_failure(failure, GlinkError::INVALID_PARAM, glink_msg::UNIT_NEEDS_SIZE, "grindUnit", object);
    }

    plan.result = r;
    plan.changed = !current.exists || r.dose_tenths != current.dose_tenths ||
                   r.has_grind_size != current.has_grind_size ||
                   (r.has_grind_size && r.grind_size_tenths != current.grind_size_tenths) ||
                   strcmp(r.grind_unit, current.grind_unit) != 0;

    if (plan.changed && !write_interval_ok) {
        return glink_set_failure(failure, GlinkError::RATE_LIMITED, glink_msg::RATE_LIMITED, "", object);
    }
    return true;
}

bool glink_decide_select(GlinkScreen screen, uint8_t index, bool profile3_exists, uint8_t current_selection,
                         bool write_interval_ok, bool* will_store, GlinkFailure& failure) {
    const int object = static_cast<int>(GlinkObject::GRIND_PROFILE);
    *will_store = false;
    if (!glink_state_check(screen, GlinkOpClass::SELECT, object, failure)) return false;
    if (index == GLINK_PROFILE_WRITABLE_INDEX && !profile3_exists) {
        return glink_set_failure(failure, GlinkError::PROFILE_NOT_SET, glink_msg::PROFILE_NOT_SET, "index", object);
    }
    if (index == current_selection) return true;
    if (!write_interval_ok) {
        return glink_set_failure(failure, GlinkError::RATE_LIMITED, glink_msg::RATE_LIMITED, "", object);
    }
    *will_store = true;
    return true;
}

GlinkError glink_start_block_error(GlinkStartBlock block, const char** message) {
    const char* unused = nullptr;
    if (message == nullptr) message = &unused;
    switch (block) {
        case GlinkStartBlock::NONE: *message = ""; return GlinkError::NONE;
        case GlinkStartBlock::MOTOR_DISABLED:
            *message = glink_msg::MOTOR_DISABLED;
            return GlinkError::MOTOR_DISABLED;
        case GlinkStartBlock::UPDATE_LOCK:
            *message = glink_msg::UPDATING;
            return GlinkError::UPDATING;
        case GlinkStartBlock::LOAD_CELL_NOT_CONNECTED:
            *message = glink_msg::LOAD_CELL_NOT_CONNECTED;
            return GlinkError::LOAD_CELL_NOT_CONNECTED;
        case GlinkStartBlock::LOAD_CELL_NO_DATA:
            *message = glink_msg::LOAD_CELL_NO_DATA;
            return GlinkError::LOAD_CELL_NO_DATA;
        case GlinkStartBlock::LOAD_CELL_RATE_INVALID:
            *message = glink_msg::LOAD_CELL_RATE_INVALID;
            return GlinkError::LOAD_CELL_RATE_INVALID;
    }
    *message = "";
    return GlinkError::NONE;
}

bool glink_decide_start(GlinkScreen screen, const GlinkStartInputs& in, GlinkFailure& failure) {
    const int object = static_cast<int>(GlinkObject::GRIND_CONTROL);
    if (!glink_state_check(screen, GlinkOpClass::START, object, failure)) return false;  // items 1 to 3
    if (!in.profile_exists) {
        return glink_set_failure(failure, GlinkError::PROFILE_NOT_SET, glink_msg::PROFILE_NOT_SET, "profile", object);
    }
    if (in.stored_dose_tenths < GLINK_DOSE_MIN_TENTHS || in.stored_dose_tenths > GLINK_DOSE_MAX_TENTHS) {
        return glink_set_failure(failure, GlinkError::OUT_OF_RANGE, glink_msg::DOSE_OUT_OF_BLE_RANGE, "dose", object);
    }
    if (in.has_expect_dose && in.expect_dose_tenths != in.stored_dose_tenths) {
        return glink_set_failure(failure, GlinkError::CONFLICT, glink_msg::CONFLICT, "expectDose", object);
    }
    const char* message = "";
    const GlinkError code = glink_start_block_error(in.block, &message);
    if (code != GlinkError::NONE) return glink_set_failure(failure, code, message, "", object);
    return true;
}

GlinkDevState glink_dev_state(GlinkScreen screen, GlinkStartBlock block) {
    switch (screen) {
        case GlinkScreen::LEAD_IN: return GlinkDevState::STARTING;
        case GlinkScreen::UPDATING: return GlinkDevState::UPDATING;
        case GlinkScreen::READY: return block == GlinkStartBlock::NONE ? GlinkDevState::IDLE : GlinkDevState::WARNING;
        case GlinkScreen::MENU:
        case GlinkScreen::EDIT:
        case GlinkScreen::OTHER: return GlinkDevState::SETTING;
        case GlinkScreen::GRINDING: return GlinkDevState::GRINDING;
        case GlinkScreen::DONE: return GlinkDevState::DONE;
        case GlinkScreen::COUNT: break;
    }
    return GlinkDevState::UNKNOWN;
}

GlinkError glink_blocked(GlinkDevState state, GlinkStartBlock block) {
    switch (state) {
        case GlinkDevState::IDLE: return GlinkError::NONE;
        case GlinkDevState::STARTING: return GlinkError::COMMAND_IN_FLIGHT;
        case GlinkDevState::UPDATING: return GlinkError::UPDATING;
        case GlinkDevState::WARNING: {
            const char* message = "";
            return glink_start_block_error(block, &message);
        }
        case GlinkDevState::GRINDING:
        case GlinkDevState::DONE:
        case GlinkDevState::SETTING:
        case GlinkDevState::UNKNOWN:
        case GlinkDevState::COUNT: break;
    }
    return GlinkError::NOT_READY;
}
