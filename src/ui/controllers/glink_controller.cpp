#include "glink_controller.h"

#include <Arduino.h>
#include <string.h>

#include "../../bluetooth/glink_gate.h"
#include "../../bluetooth/manager.h"
#include "../../bluetooth/glink_screen.h"
#include "../../bluetooth/glink_session_map.h"
#include "../../bluetooth/glink_start.h"
#include "../../config/constants.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../hardware/WeightSensor.h"
#include "../../hardware/display_manager.h"
#include "../../network/portal_command_gate.h"
#include "../../network/portal_mailbox.h"
#include "../../system/update_guard.h"
#include "../ui_manager.h"
#include "autotune_controller.h"

// The wire phase mapping relies on these GrindPhase numbers (bluetooth/glink_screen.cpp)
static_assert(static_cast<int>(GrindPhase::IDLE) == 0 && static_cast<int>(GrindPhase::INITIALIZING) == 1 &&
                  static_cast<int>(GrindPhase::TIME_ADDITIONAL_PULSE) == 11 &&
                  static_cast<int>(GrindPhase::COMPLETED) == 12 && static_cast<int>(GrindPhase::TIMEOUT) == 13 &&
                  static_cast<int>(GrindPhase::PRIME) == 14 && static_cast<int>(GrindPhase::PURGE_CONFIRM) == 16,
              "GrindPhase order changed: update glink_phase_from_grind in bluetooth/glink_screen.cpp");
static_assert(USER_DOSE_PROFILE_INDEX == GLINK_PROFILE_WRITABLE_INDEX && USER_PROFILE_COUNT == GLINK_PROFILE_COUNT_MAX,
              "GLink profile indices differ from the profile controller");

namespace {

constexpr int kDoseIndex = GLINK_PROFILE_WRITABLE_INDEX;

bool same_entry(const GlinkProfileEntry& a, const GlinkProfileEntry& b) {
    return a.dose_tenths == b.dose_tenths && a.has_grind_size == b.has_grind_size &&
           a.grind_size_tenths == b.grind_size_tenths && strcmp(a.grind_unit, b.grind_unit) == 0;
}

GlinkMailboxResult failure_result(GlinkError code, const char* message, const char* field = "") {
    GlinkMailboxResult result;
    result.kind = GlinkReplyKind::FAILURE;
    glink_set_failure(result.failure, code, message, field, static_cast<int>(GlinkObject::GRIND_PROFILE));
    return result;
}

}  // namespace

GlinkUIController::GlinkUIController(UIManager* manager) : ui_manager_(manager) {}

GlinkUIController::~GlinkUIController() {
    // Nobody serves the mailbox or refreshes the snapshot any more: reads and requests fail with UI_BUSY
    glink_command_gate.attach_ui(false);
}

void GlinkUIController::update() {
    if (!ui_manager_ || !ui_manager_->state_machine || !ui_manager_->profile_controller) return;

    // First frame: the UI exists and is polled, so requests can be answered from here on
    if (!attached_) {
        attached_ = true;
        stop_seen_ = portal_command_gate.stop_seq();  // a stop from before the UI existed is not ours to replay
        if (ui_manager_->grind_controller) seen_result_count_ = ui_manager_->grind_controller->result_count();
        glink_command_gate.attach_ui(true);
    }

    // A stop is never queued: with no lead-in to cancel it is consumed here, so it cannot cancel a start accepted later
    if (glink_consume_idle_stop(lead_in_.active, glink_command_gate.request_queued())) {
        portal_stop_pending(portal_command_gate.stop_seq(), &stop_seen_);
    }

    serve_mailbox();
    run_lead_in();  // before the link check below: a lead-in that began runs to its end whether or not the client is still there
    watch_result();
    // Profile changes by touch, web panel or reset are counted whether or not a client is linked, so profileRev never
    // misses one a client could have slept through
    fill_profiles(&snapshot_);
    count_profile_changes(snapshot_);

    auto* bt = ui_manager_->bluetooth_manager;
    const bool linked = bt && bt->is_connected();
    const bool just_linked = linked && !linked_;
    linked_ = linked;
    if (!linked) return;

    const uint32_t now = millis();
    const UIState state = ui_manager_->state_machine->get_current_state();
    const bool state_changed = state != last_state_;
    last_state_ = state;
    if (just_linked || state_changed || static_cast<uint32_t>(now - last_publish_ms_) >= BLE_GLINK_SNAPSHOT_PUBLISH_MS) {
        publish_snapshot(now);
    }
}

//------------------------------------------------------------------------------
// Snapshot
//------------------------------------------------------------------------------
GlinkScreen GlinkUIController::classify_screen(bool ignore_own_lead_in) const {
    auto* hardware = ui_manager_->hardware_manager;
    auto* grinder = hardware ? hardware->get_grinder() : nullptr;
    auto* autotune = hardware ? hardware->get_autotune_controller() : nullptr;
    auto* bt = ui_manager_->bluetooth_manager;
    auto* update_ui = ui_manager_->wifi_update_controller_.get();
    auto* portal = ui_manager_->portal_controller_.get();

    GlinkStateInputs in;
    in.ui_state = ui_manager_->state_machine->get_current_state();
    // Our own lead-in overlay is left out when the lead-in asks whether a start would still be allowed
    in.overlay_active = !ignore_own_lead_in && BlockingOperationOverlay::getInstance().is_operation_active();
    in.busy = (ui_manager_->grind_controller && ui_manager_->grind_controller->is_grind_running()) ||
              (autotune && autotune->is_active()) || (grinder && grinder->is_grinding());
    in.update_active = UpdateGuard::is_held() || (bt && bt->is_updating()) ||
                       (update_ui && (update_ui->progress_shown() || update_ui->prompt_shown()));
    // The web panel's lead-in always counts (the two are exclusive; if both ever ran, ours would be dropped)
    in.lead_in_active = (!ignore_own_lead_in && lead_in_.active) || (portal && portal->lead_in_active());
    return glink_classify_screen(in);
}

GlinkProfileState GlinkUIController::dose_profile_state() const {
    auto* profiles = ui_manager_->profile_controller;
    GlinkProfileState state;
    state.exists = profiles->has_dose_profile();
    if (!state.exists) return state;
    state.dose_tenths = glink_tenths_from_float(profiles->get_profile_weight(kDoseIndex));
    const GrindSize& size = profiles->get_grind_size(kDoseIndex);
    state.has_grind_size = size.present;
    state.grind_size_tenths = size.present ? size.tenths : 0;
    if (size.present) strncpy(state.grind_unit, size.unit, sizeof(state.grind_unit) - 1);
    return state;
}

// The four profiles as the wire shows them; entry 3 is all zero until the Profile Dose exists
void GlinkUIController::fill_profiles(GlinkSnapshot* out) const {
    auto* profiles = ui_manager_->profile_controller;
    out->profile3_exists = profiles->has_dose_profile();
    for (int i = 0; i < GLINK_PROFILE_COUNT_MAX; i++) {
        GlinkProfileEntry& entry = out->profiles[i];
        entry = GlinkProfileEntry();
        entry.index = static_cast<uint8_t>(i);
        if (!profiles->profile_exists(i)) continue;
        entry.dose_tenths = glink_tenths_from_float(profiles->get_profile_weight(i));
        const GrindSize& size = profiles->get_grind_size(i);
        if (size.present) {
            entry.has_grind_size = true;
            entry.grind_size_tenths = size.tenths;
            strncpy(entry.grind_unit, size.unit, sizeof(entry.grind_unit) - 1);
        }
    }
}

void GlinkUIController::count_profile_changes(const GlinkSnapshot& now) {
    if (profiles_seen_) {
        bool changed = now.profile3_exists != last_exists_;
        for (int i = 0; i < GLINK_PROFILE_COUNT_MAX && !changed; i++) {
            changed = !same_entry(now.profiles[i], last_profiles_[i]);
        }
        if (changed) profile_rev_++;
    }
    for (int i = 0; i < GLINK_PROFILE_COUNT_MAX; i++) last_profiles_[i] = now.profiles[i];
    last_exists_ = now.profile3_exists;
    profiles_seen_ = true;
}

void GlinkUIController::publish_snapshot(uint32_t now_ms) {
    auto* grind = ui_manager_->grind_controller;
    auto* hardware = ui_manager_->hardware_manager;
    auto* sensor = hardware ? hardware->get_weight_sensor() : nullptr;

    snapshot_.screen = classify_screen();

    GlinkPeriodInputs in;
    in.screen = snapshot_.screen;
    in.block = glink_start_block_from_grind(grind ? grind->start_block() : GrindStartBlock::MOTOR_DISABLED);
    in.grind_phase_id = grind ? grind->phase_value() : 0;
    in.weight_valid = sensor && !sensor->has_hardware_fault() && !sensor->is_sample_stale();
    in.weight_g = in.weight_valid ? sensor->get_display_weight() : 0.0f;
    in.select_profile = static_cast<uint8_t>(ui_manager_->profile_controller->get_current_profile());
    if (grind) {
        in.time_mode = grind->get_mode() == GrindMode::TIME;
        in.target = in.time_mode ? static_cast<float>(grind->get_target_time_ms()) / 1000.0f : grind->get_target_weight();
        in.result_seq = grind->result_count();
    }
    in.profile_rev = profile_rev_;
    fill_lead_in_period(&in, now_ms);
    snapshot_.period = glink_build_period(in);

    glink_command_gate.publish(snapshot_);
    last_publish_ms_ = now_ms;
}

//------------------------------------------------------------------------------
// Mailbox: grindProfile update and select
//------------------------------------------------------------------------------
void GlinkUIController::serve_mailbox() {
    GlinkRequest request;
    if (glink_command_gate.take(&request)) handle_request(request);
}

void GlinkUIController::handle_request(const GlinkRequest& request) {
    const uint32_t now = millis();
    const GlinkScreen screen = classify_screen();
    GlinkMailboxResult result;
    switch (request.op) {
        case GlinkOp::UPDATE: result = apply_update(request, screen, now); break;
        case GlinkOp::SELECT: result = apply_select(request, screen, now); break;
        case GlinkOp::START: result = apply_start(request, screen); break;
        default:  // stop is executed on the Bluetooth task and never reaches the mailbox
            result = failure_result(GlinkError::UNKNOWN_OP, glink_msg::OP_UNSUPPORTED);
            break;
    }

    // The next get must see what was just written: refresh the copy before the answer is released
    fill_profiles(&snapshot_);
    count_profile_changes(snapshot_);
    if (linked_) publish_snapshot(now);
    LOG_BLE("[GLINK] %s %s\n", glink_op_name(request.op),
            result.kind == GlinkReplyKind::FAILURE ? glink_error_code(result.failure.code) : "ok");
    glink_command_gate.complete(result);
}

// Writes the Profile Dose so that a failure part-way leaves what was there before (best effort for the grind size)
bool GlinkUIController::store_dose_profile(const GlinkProfileEntry& wanted, const GlinkProfileState& before) {
    auto* profiles = ui_manager_->profile_controller;
    const float dose = static_cast<float>(wanted.dose_tenths) / 10.0f;
    GrindSize size;
    size.present = wanted.has_grind_size;
    if (size.present) {
        size.tenths = wanted.grind_size_tenths;
        strncpy(size.unit, wanted.grind_unit, sizeof(size.unit) - 1);
    }

    if (!before.exists) {
        // The first dose creates the profile (and clears any size left from an earlier one); the size follows
        if (!profiles->store_profile_weight(kDoseIndex, dose)) return false;
        if (size.present && !profiles->store_grind_size(kDoseIndex, size)) {
            profiles->remove_dose_profile();
            return false;
        }
        return true;
    }

    const GrindSize old_size = profiles->get_grind_size(kDoseIndex);
    const bool size_changed = !grind_size_equal(old_size, size);
    const bool dose_changed = wanted.dose_tenths != before.dose_tenths;
    if (size_changed && !profiles->store_grind_size(kDoseIndex, size)) return false;
    if (dose_changed && !profiles->store_profile_weight(kDoseIndex, dose)) {
        if (size_changed) profiles->store_grind_size(kDoseIndex, old_size);
        return false;
    }
    return true;
}

GlinkMailboxResult GlinkUIController::apply_update(const GlinkRequest& request, GlinkScreen screen, uint32_t now_ms) {
    GlinkMailboxResult result;
    const GlinkProfileState before = dose_profile_state();
    GlinkUpdatePlan plan;
    if (!glink_decide_update(screen, before, request, limiter_.ok(now_ms), plan, result.failure)) {
        result.kind = GlinkReplyKind::FAILURE;
        return result;
    }

    result.kind = GlinkReplyKind::PROFILE_UPDATE;
    result.entry = plan.result;
    if (!plan.changed) return result;  // success without a flash write (changed and stored false)

    limiter_.note_store(now_ms);  // a failed attempt counts too: the flash was touched
    if (!store_dose_profile(plan.result, before)) {
        return failure_result(GlinkError::STORE_FAILED, glink_msg::STORE_FAILED);
    }

    // Same refresh the touchscreen does after a value changed; sync_pages() adds or removes the PROFILE page later in
    // the frame
    if (ui_manager_->ready_controller_) ui_manager_->ready_controller_->refresh_profiles();

    fill_profiles(&snapshot_);
    result.entry = snapshot_.profiles[kDoseIndex];  // what is stored now
    result.changed = true;
    result.stored = true;
    return result;
}

GlinkMailboxResult GlinkUIController::apply_select(const GlinkRequest& request, GlinkScreen screen, uint32_t now_ms) {
    GlinkMailboxResult result;
    auto* profiles = ui_manager_->profile_controller;
    bool will_store = false;
    if (!glink_decide_select(screen, request.index, profiles->has_dose_profile(), profiles->get_current_profile(),
                             limiter_.ok(now_ms), &will_store, result.failure)) {
        result.kind = GlinkReplyKind::FAILURE;
        return result;
    }
    // The PROFILE page appears or goes away once the screen has been idle (sync_pages): until the READY screen matches the
    // stored profiles the tab cannot be shown, so the client retries shortly. Nothing is stored, no rate-limit slot used.
    if (ui_manager_->ready_screen.has_dose_page() != profiles->has_dose_profile() || !ui_manager_->ready_controller_) {
        return failure_result(GlinkError::UI_BUSY, glink_msg::UI_BUSY);
    }
    if (will_store) limiter_.note_store(now_ms);

    const int index = request.index;
    if (index < ui_manager_->menu_tab_index()) {
        // READY (the decision guarantees it): show the profile page and let the tab handler store the selection
        // (compare-first), refresh the values and the button icon, exactly as a swipe would
        ui_manager_->ready_screen.set_active_tab(index);
        ui_manager_->ready_controller_->handle_tab_change(index);
    } else {
        profiles->set_current_profile(index);  // the MENU tab cannot be selected (index <= 3), kept as a safe fallback
    }

    if (profiles->get_current_profile() != index) {
        return failure_result(GlinkError::STORE_FAILED, glink_msg::STORE_FAILED);
    }
    result.kind = GlinkReplyKind::SELECT;
    result.select_profile = static_cast<uint8_t>(index);
    return result;
}

//------------------------------------------------------------------------------
// Start: grindControl start, lead-in, result
//------------------------------------------------------------------------------
GlinkStartInputs GlinkUIController::gather_start_inputs(int profile, bool has_expect, int32_t expect_tenths) const {
    auto* profiles = ui_manager_->profile_controller;
    auto* grind = ui_manager_->grind_controller;
    GlinkStartInputs in;
    in.profile_exists = profiles->profile_exists(profile);
    in.stored_dose_tenths = in.profile_exists ? glink_tenths_from_float(profiles->get_profile_weight(profile)) : 0;
    in.has_expect_dose = has_expect;
    in.expect_dose_tenths = expect_tenths;
    in.block = glink_start_block_from_grind(grind ? grind->start_block() : GrindStartBlock::MOTOR_DISABLED);
    return in;
}

// Step 1 of a start, on the UI task: the whole decision of 7.2 on the real state, then the lead-in begins and the
// request is answered (accepted). The grind itself starts when the lead-in ends (run_lead_in).
GlinkMailboxResult GlinkUIController::apply_start(const GlinkRequest& request, GlinkScreen screen) {
    GlinkMailboxResult result;
    const GlinkStartInputs in = gather_start_inputs(request.index, request.has_expect_dose, request.expect_dose_tenths);
    if (!glink_decide_start(screen, in, result.failure)) {
        result.kind = GlinkReplyKind::FAILURE;
        LOG_BLE("[GLINK] start refused: %s\n", glink_error_code(result.failure.code));
        return result;
    }

    lead_in_profile_ = request.index;
    lead_in_has_expect_ = request.has_expect_dose;
    lead_in_expect_tenths_ = request.expect_dose_tenths;
    portal_lead_in_begin(&lead_in_, millis());
    lead_in_shown_s_ = portal_lead_in_remaining_s(lead_in_, millis());
    char text[80];
    glink_lead_in_text(text, sizeof(text), ui_manager_->profile_controller->get_profile_name(lead_in_profile_),
                       in.stored_dose_tenths, lead_in_shown_s_);
    if (ui_manager_->screen_timeout_controller_) ui_manager_->screen_timeout_controller_->hold_awake_remote_start(true);
    PortalLeadIn* lead_in = &lead_in_;  // the overlay's STOP button cancels like a touch anywhere
    BlockingOperationOverlay::getInstance().show_with_stop(text, [lead_in]() { portal_lead_in_cancel(lead_in); });
    LOG_BLE("[GLINK] remote start: profile %d, grind starts in %u ms unless cancelled\n", lead_in_profile_,
            static_cast<unsigned>(GLINK_START_LEAD_IN_MS));

    result.kind = GlinkReplyKind::START;
    result.lead_in_ms = GLINK_START_LEAD_IN_MS;
    return result;
}

void GlinkUIController::end_lead_in() {
    if (ui_manager_->screen_timeout_controller_) ui_manager_->screen_timeout_controller_->hold_awake_remote_start(false);
    BlockingOperationOverlay::getInstance().hide();
    lead_in_ = PortalLeadIn();
}

// Step 2: counts the lead-in down; any touch, the overlay's STOP, a stop request, the link being switched off or a
// state that no longer allows a start ends it. At GO the decision is made again.
void GlinkUIController::run_lead_in() {
    if (!lead_in_.active) return;
    const uint32_t now = millis();

    auto* hardware = ui_manager_->hardware_manager;
    auto* display = hardware ? hardware->get_display() : nullptr;
    auto* touch = display ? display->get_touch_driver() : nullptr;
    auto* bt = ui_manager_->bluetooth_manager;

    GlinkLeadInInputs in;
    in.touch_available = touch != nullptr;
    in.ms_since_last_touch = touch ? touch->get_ms_since_last_touch() : 0;
    in.stop_pending = portal_stop_pending(portal_command_gate.stop_seq(), &stop_seen_);
    in.link_enabled = bt && bt->link_mode() != GlinkLinkMode::OFF;
    // The same decision a start request gets, on the state without our own overlay; a start block (fault, update lock)
    // that appears now drops the start silently
    GlinkFailure refused;
    in.start_allowed = glink_decide_start(classify_screen(true),
                                          gather_start_inputs(lead_in_profile_, lead_in_has_expect_, lead_in_expect_tenths_),
                                          refused);

    switch (glink_lead_in_step(&lead_in_, in, now)) {
        case GlinkLeadInEvent::WAITING: {
            const uint32_t remaining_s = portal_lead_in_remaining_s(lead_in_, now);
            if (remaining_s != lead_in_shown_s_) {
                lead_in_shown_s_ = remaining_s;
                auto* profiles = ui_manager_->profile_controller;
                char text[80];
                glink_lead_in_text(text, sizeof(text), profiles->get_profile_name(lead_in_profile_),
                                   glink_tenths_from_float(profiles->get_profile_weight(lead_in_profile_)), remaining_s);
                BlockingOperationOverlay::getInstance().set_message(text);
            }
            return;
        }
        case GlinkLeadInEvent::GO:
            end_lead_in();
            start_after_lead_in();
            return;
        case GlinkLeadInEvent::DROPPED_STATE:
            end_lead_in();
            LOG_BLE("[GLINK] remote start dropped: %s\n", glink_error_code(refused.code));
            return;
        case GlinkLeadInEvent::DROPPED_LINK_OFF:
            end_lead_in();
            LOG_BLE("[GLINK] remote start dropped: link switched off\n");
            return;
        case GlinkLeadInEvent::CANCELLED_TOUCH:
        case GlinkLeadInEvent::CANCELLED_NO_TOUCH:
        case GlinkLeadInEvent::CANCELLED_STOP:
        case GlinkLeadInEvent::CANCELLED_USER:
            end_lead_in();
            LOG_BLE("[GLINK] remote start cancelled\n");
            return;
        case GlinkLeadInEvent::NONE:
            return;
    }
}

// Time is up: the overlay is gone (it classifies the screen as busy), the decision is made again on what is true now,
// and the grind starts through the shared start body.
void GlinkUIController::start_after_lead_in() {
    auto* grind = ui_manager_->grind_controller;
    auto* grinding = ui_manager_->grinding_controller_.get();
    if (!grind || !grinding) return;

    const GlinkScreen screen = classify_screen();
    const GlinkStartInputs in = gather_start_inputs(lead_in_profile_, lead_in_has_expect_, lead_in_expect_tenths_);
    GlinkFailure refused;
    if (!glink_decide_start(screen, in, refused)) {
        LOG_BLE("[GLINK] remote start refused: %s\n", glink_error_code(refused.code));
        // Only a start block opens the confirmation dialog (5.5); any other refusal shows nothing
        if (glink_refusal_shows_dialog(screen, in, refused)) ui_manager_->show_start_blocked_notice(grind->start_block());
        return;
    }
    LOG_BLE("[GLINK] remote start: starting grind, profile %d\n", lead_in_profile_);
    const GrindStartBlock block = grinding->start_glink_grind(lead_in_profile_);
    if (block == GrindStartBlock::NONE) {
        ble_session_id_ = grind->session_id();
        return;
    }
    // start_glink_grind() also refuses (generic MOTOR_DISABLED) for a dose outside 5 to 60 g, which the decision above
    // already excluded; the motor dialog opens only for a block the grind controller still reports
    LOG_BLE("[GLINK] remote start refused by the grind controller\n");
    if (grind->start_block() == block) ui_manager_->show_start_blocked_notice(block);
}

// What the Bluetooth task needs while STARTING: the time left and the target (5.3)
void GlinkUIController::fill_lead_in_period(GlinkPeriodInputs* in, uint32_t now_ms) const {
    if (in->screen != GlinkScreen::LEAD_IN) return;
    auto* profiles = ui_manager_->profile_controller;
    auto* portal = ui_manager_->portal_controller_.get();
    if (lead_in_.active) {
        in->lead_in_known = true;
        in->lead_in_ms = glink_lead_in_remaining_ms(lead_in_, now_ms);
        in->time_mode = false;
        in->target = profiles->get_profile_weight(lead_in_profile_);
    } else if (portal && portal->lead_in_active()) {
        // The web panel's start grinds the profile target in the grinder's current mode
        in->lead_in_known = true;
        in->lead_in_ms = glink_lead_in_remaining_ms(portal->lead_in_state(), now_ms);
        in->time_mode = ui_manager_->current_mode == GrindMode::TIME;
        in->target = get_profile_target(*profiles, ui_manager_->current_mode, portal->lead_in_profile());
    }
}

// Every finished grind (a normal end, an abort or a stop), whoever started it: read through the seam when
// result_count() moved, mapped to the wire result and published in the snapshot for the Bluetooth task.
void GlinkUIController::watch_result() {
    auto* grind = ui_manager_->grind_controller;
    if (!grind) return;
    const uint32_t count = grind->result_count();
    if (count == seen_result_count_) return;

    GrindController::GrindResult raw;
    if (!grind->read_last_result(&raw)) return;  // a writer was inside: try again on the next frame
    if (grind->result_count() != count) return;  // another result arrived meanwhile: the seq would not match

    const GrindSessionDescriptor& session = grind->get_session_descriptor();
    auto* portal = ui_manager_->portal_controller_.get();
    GlinkGrindRecord record;
    record.seq = count;
    record.outcome = static_cast<uint8_t>(raw.outcome);
    record.abort_reason = raw.abort_reason;
    record.stopped = raw.stopped;
    record.final_weight_g = raw.final_weight_g;
    record.pulses = raw.pulses;
    record.total_time_ms = raw.total_time_ms;
    record.motor_time_ms = raw.motor_time_ms;
    record.profile_id = session.profile_id;
    record.time_mode = session.mode == GrindMode::TIME;
    record.target_weight_g = session.target_weight;
    record.target_time_ms = session.target_time_ms;
    record.origin = glink_origin_of_session(raw.session_id, ble_session_id_, portal ? portal->web_session_id() : 0);

    snapshot_.last_result = glink_result_from_record(record);
    snapshot_.has_result = true;
    seen_result_count_ = count;
    LOG_BLE("[GLINK] grind result %u: %s (%s)\n", static_cast<unsigned>(count),
            glink_result_code_name(snapshot_.last_result.code), glink_origin_name(snapshot_.last_result.origin));
    if (linked_) publish_snapshot(millis());
}
