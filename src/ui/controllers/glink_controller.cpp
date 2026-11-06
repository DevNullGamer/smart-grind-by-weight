#include "glink_controller.h"

#include <Arduino.h>
#include <string.h>

#include "../../bluetooth/glink_gate.h"
#include "../../bluetooth/manager.h"
#include "../../bluetooth/glink_screen.h"
#include "../../config/constants.h"
#include "../../hardware/WeightSensor.h"
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
        glink_command_gate.attach_ui(true);
    }

    serve_mailbox();
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
GlinkScreen GlinkUIController::classify_screen() const {
    auto* hardware = ui_manager_->hardware_manager;
    auto* grinder = hardware ? hardware->get_grinder() : nullptr;
    auto* autotune = hardware ? hardware->get_autotune_controller() : nullptr;
    auto* bt = ui_manager_->bluetooth_manager;
    auto* update_ui = ui_manager_->wifi_update_controller_.get();
    auto* portal = ui_manager_->portal_controller_.get();

    GlinkStateInputs in;
    in.ui_state = ui_manager_->state_machine->get_current_state();
    in.overlay_active = BlockingOperationOverlay::getInstance().is_operation_active();
    in.busy = (ui_manager_->grind_controller && ui_manager_->grind_controller->is_grind_running()) ||
              (autotune && autotune->is_active()) || (grinder && grinder->is_grinding());
    in.update_active = UpdateGuard::is_held() || (bt && bt->is_updating()) ||
                       (update_ui && (update_ui->progress_shown() || update_ui->prompt_shown()));
    in.lead_in_active = portal && portal->lead_in_active();
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
        default: result = failure_result(GlinkError::UNKNOWN_OP, glink_msg::OP_UNSUPPORTED); break;
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
