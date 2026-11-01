#include "portal_ui_controller.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>

#include "../../config/constants.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../network/portal_command_gate.h"
#include "../../network/wifi_manager.h"
#include "../../system/menu_setting_models.h"
#include "../../system/menu_setting_table.h"
#include "../../system/nvs_settings_store.h"
#include "../../system/update_guard.h"
#include "../ui_manager.h"

// The API field order is the menu table order, and the pure snapshot module names grind phases by position
static_assert(PF_BLE_STARTUP == static_cast<size_t>(ToggleSetting::BLE_STARTUP), "portal field order != toggle order");
static_assert(PF_LOGGING == static_cast<size_t>(ToggleSetting::LOGGING), "portal field order != toggle order");
static_assert(PF_SWIPE == static_cast<size_t>(ToggleSetting::SWIPE), "portal field order != toggle order");
static_assert(PF_AUTO_START == static_cast<size_t>(ToggleSetting::AUTO_START), "portal field order != toggle order");
static_assert(PF_AUTO_RETURN == static_cast<size_t>(ToggleSetting::AUTO_RETURN), "portal field order != toggle order");
static_assert(PF_AUTO_RETURN + 1 == PF_BRIGHTNESS_NORMAL, "portal field order changed");
static_assert(static_cast<int>(GrindPhase::IDLE) == 0 && static_cast<int>(GrindPhase::TARING) == 3 &&
                  static_cast<int>(GrindPhase::PREDICTIVE) == 5 && static_cast<int>(GrindPhase::TIME_GRINDING) == 10 &&
                  static_cast<int>(GrindPhase::COMPLETED) == 12 && static_cast<int>(GrindPhase::PRIME) == 14 &&
                  static_cast<int>(GrindPhase::PURGE_CONFIRM) == static_cast<int>(PORTAL_GRIND_PHASE_COUNT) - 1,
              "GrindPhase order changed: update kGrindPhaseNames in network/portal_snapshot.cpp");

namespace {

// Brightness percent, purge slider units or freshness step of a normalised API value, as the slider handler
// of the menu would hold them
int slider_position(SliderSetting id, float value) {
    switch (id) {
        case SliderSetting::PURGE_AMOUNT: return purge_slider_from_amount(value);
        case SliderSetting::FRESHNESS: return freshness_index_from_hours(value);
        default: return static_cast<int>(lroundf(value));
    }
}

SliderSetting slider_of_field(size_t index) {
    switch (index) {
        case PF_BRIGHTNESS_NORMAL: return SliderSetting::BRIGHTNESS_NORMAL;
        case PF_BRIGHTNESS_SCREENSAVER: return SliderSetting::BRIGHTNESS_SCREENSAVER;
        case PF_PURGE_AMOUNT_G: return SliderSetting::PURGE_AMOUNT;
        default: return SliderSetting::FRESHNESS;  // PF_FRESHNESS_HOURS
    }
}

// (not "bit": Arduino.h defines a macro of that name)
constexpr uint32_t field_bit(size_t field) { return 1u << field; }

}  // namespace

PortalUIController::PortalUIController(UIManager* manager) : ui_manager_(manager) {}

PortalUIController::~PortalUIController() {
    // Nobody serves the mailbox or refreshes the snapshot any more: reads and writes fail with ui_busy
    portal_command_gate.attach_ui(false);
}

void PortalUIController::update() {
    if (!ui_manager_ || !ui_manager_->state_machine) return;

    // First frame: the UI exists and is polled, so requests can be answered from here on
    if (!attached_) {
        attached_ = true;
        portal_command_gate.attach_ui(true);
    }

    serve_mailbox();

    // Nobody reads the snapshot while the web routes are closed: no work, and no NVS reads
    if (!wifi_manager.web_enabled()) {
        web_was_enabled_ = false;
        return;
    }
    const uint32_t now = millis();
    const bool just_enabled = !web_was_enabled_;
    web_was_enabled_ = true;
    // Leaving the menu: the user may have changed settings there, read them now instead of waiting
    const UIState state = ui_manager_->state_machine->get_current_state();
    const bool left_menu = last_state_ == UIState::MENU && state != UIState::MENU;
    last_state_ = state;
    if (just_enabled || left_menu || publish_due_ ||
        static_cast<uint32_t>(now - last_publish_ms_) >= PORTAL_STATUS_PUBLISH_MS) {
        publish_snapshot(now, just_enabled || left_menu);
    }
}

//------------------------------------------------------------------------------
// Snapshot
//------------------------------------------------------------------------------
void PortalUIController::publish_snapshot(uint32_t now_ms, bool reload_values) {
    // The stored settings change from the touchscreen without telling us: re-read them now and then
    if (reload_values || !values_valid_ || static_cast<uint32_t>(now_ms - values_loaded_ms_) >= PORTAL_VALUES_REFRESH_MS) {
        load_values(&values_);
        values_valid_ = true;
        values_loaded_ms_ = now_ms;
    }
    portal_command_gate.publish(values_, portal_build_status(gather_status()));
    last_publish_ms_ = now_ms;
    publish_due_ = false;
}

void PortalUIController::load_values(PortalValues* out) const {
    *out = PortalValues();
    portal_load_settings(ui_settings, out);
    auto* profiles = ui_manager_->profile_controller;
    if (profiles) {
        for (int p = 0; p < static_cast<int>(PORTAL_PROFILE_COUNT); p++) {
            out->v[PF_SINGLE_WEIGHT_G + 2 * p] = profiles->get_profile_weight(p);
            out->v[PF_SINGLE_TIME_S + 2 * p] = profiles->get_profile_time(p);
        }
        out->v[PF_GRIND_MODE] = static_cast<float>(grind_mode_radio_index(static_cast<int>(profiles->get_grind_mode())));
        out->current_profile = profiles->get_current_profile();
    }
    portal_device_name_from_host(wifi_manager.hostname(), out->device_name);
}

bool PortalUIController::grinder_busy() const {
    auto* hardware = ui_manager_->hardware_manager;
    auto* grinder = hardware ? hardware->get_grinder() : nullptr;
    auto* autotune = hardware ? hardware->get_autotune_controller() : nullptr;
    return (ui_manager_->grind_controller && ui_manager_->grind_controller->is_grind_running()) ||
           (autotune && autotune->is_active()) || (grinder && grinder->is_grinding());
}

PortalStatusInputs PortalUIController::gather_status() const {
    PortalStatusInputs in;
    auto* grind = ui_manager_->grind_controller;
    auto* sensor = ui_manager_->hardware_manager ? ui_manager_->hardware_manager->get_weight_sensor() : nullptr;
    in.ui_state = ui_manager_->state_machine->get_current_state();
    in.grind_phase_id = grind ? grind->phase_value() : 0;
    in.busy = grinder_busy();
    in.weight_valid = sensor && !sensor->has_hardware_fault() && !sensor->is_sample_stale();
    in.weight_g = in.weight_valid ? sensor->get_display_weight() : 0.0f;
    in.motor_available = ui_manager_->motor_available();
    in.start_block = grind ? grind->start_block() : GrindStartBlock::MOTOR_DISABLED;
    in.motor_test_block = ui_manager_->motor_pulse_block();
    in.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    in.build = BUILD_NUMBER;
    return in;
}

PortalDecisionInput PortalUIController::gather_decision() const {
    PortalDecisionInput in;
    in.ui = portal_ui_mode(ui_manager_->state_machine->get_current_state(),
                           BlockingOperationOverlay::getInstance().is_operation_active());
    in.busy = grinder_busy();
    in.update_held = UpdateGuard::is_held();
    in.command_in_flight = false;  // the mailbox itself admits one request at a time
    return in;
}

//------------------------------------------------------------------------------
// Mailbox: PATCH /api/settings
//------------------------------------------------------------------------------
void PortalUIController::serve_mailbox() {
    PortalPatch patch;
    if (portal_command_gate.take_patch(&patch)) handle_patch(patch);
}

void PortalUIController::handle_patch(const PortalPatch& patch) {
    const PortalError refused = portal_write_decide(gather_decision());
    if (refused != PortalError::NONE) {
        PortalMailboxResult result;
        result.error = refused;
        result.adjusted_mask = patch.adjusted;
        portal_command_gate.complete(result);
        LOG_BLE("[PORTAL] settings write refused: %s\n", portal_error_info(refused).code);
        return;
    }

    PortalValues current;
    load_values(&current);
    const PortalApplyOutcome outcome = portal_apply_patch(patch, current, *this);
    apply_to_screen(outcome.written_mask);

    // The answer is the stored state after the write (also after a failure: what did get applied)
    load_values(&values_);
    values_valid_ = true;
    values_loaded_ms_ = millis();
    publish_due_ = true;
    portal_command_gate.complete(portal_result_from_outcome(outcome, patch.adjusted, values_));
    LOG_BLE("[PORTAL] settings write: fields 0x%05lx written%s%s\n", static_cast<unsigned long>(outcome.written_mask),
            outcome.device_name_changed ? ", device name changed" : "",
            outcome.error == PortalError::NONE ? "" : ", STORE FAILED");
}

bool PortalUIController::write_field(size_t index, float value) {
    auto* profiles = ui_manager_->profile_controller;
    if (index <= PF_AUTO_RETURN) {
        const ToggleSettingDef& def = toggle_setting_def(static_cast<ToggleSetting>(index));
        return (ui_settings.*def.set)(value >= 0.5f);
    }
    switch (index) {
        case PF_BRIGHTNESS_NORMAL:
        case PF_BRIGHTNESS_SCREENSAVER:
        case PF_PURGE_AMOUNT_G:
        case PF_FRESHNESS_HOURS: {
            const SliderSetting id = slider_of_field(index);
            return slider_setting_def(id).store(ui_settings, slider_position(id, value));
        }
        case PF_PURGE_MODE:
            return ui_settings.set_purge_mode(value >= 0.5f ? 1 : 0);
        case PF_GRIND_MODE: {
            if (!profiles) return false;
            // Same as the menu's radio handler: the profile controller stores it, the UI keeps its own copy
            const GrindMode mode = value >= 0.5f ? GrindMode::TIME : GrindMode::WEIGHT;
            if (!profiles->set_grind_mode(mode)) return false;  // RAM kept the old mode: so does the UI
            ui_manager_->current_mode = mode;
            return true;
        }
        default:
            break;
    }
    if (!profiles || index < PF_SINGLE_WEIGHT_G || index > PF_CUSTOM_TIME_S) return false;
    const int profile = static_cast<int>((index - PF_SINGLE_WEIGHT_G) / 2);
    const bool is_weight = ((index - PF_SINGLE_WEIGHT_G) % 2) == 0;
    return is_weight ? profiles->set_profile_weight(profile, value) : profiles->set_profile_time(profile, value);
}

bool PortalUIController::write_device_name(const char* name, bool* changed) {
    return wifi_manager.set_device_name(name, changed);
}

// What the touchscreen handlers do after storing the same values, for the fields that were written
void PortalUIController::apply_to_screen(uint32_t written_mask) {
    if (written_mask == 0) return;

    if (written_mask & (field_bit(PF_BRIGHTNESS_NORMAL) | field_bit(PF_BRIGHTNESS_SCREENSAVER))) {
        if (ui_manager_->screen_timeout_controller_) ui_manager_->screen_timeout_controller_->reapply_brightness();
    }
    if (written_mask & (field_bit(PF_AUTO_START) | field_bit(PF_AUTO_RETURN))) {
        ui_manager_->refresh_auto_action_settings();
    }
    if (written_mask & field_bit(PF_GRIND_MODE)) {
        if (ui_manager_->ready_controller_) ui_manager_->ready_controller_->refresh_profiles();
        if (ui_manager_->profile_controller) {
            ui_manager_->edit_target = get_current_profile_target(*ui_manager_->profile_controller, ui_manager_->current_mode);
        }
        ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
        if (ui_manager_->grinding_controller_) ui_manager_->grinding_controller_->update_grind_button_icon();
    }
    constexpr uint32_t kProfileBits = ((1u << PORTAL_PROFILE_FIELD_COUNT) - 1) << PF_SINGLE_WEIGHT_G;
    if (written_mask & kProfileBits) {
        if (ui_manager_->ready_controller_) ui_manager_->ready_controller_->refresh_profiles();
    }
    // An open menu page shows the new values (the pages are also refreshed whenever the menu is shown)
    if (ui_manager_->state_machine->is_state(UIState::MENU)) {
        ui_manager_->menu_screen.refresh_setting_toggles();
        ui_manager_->menu_screen.refresh_setting_sliders();
        ui_manager_->menu_screen.refresh_setting_radios();
    }
}
