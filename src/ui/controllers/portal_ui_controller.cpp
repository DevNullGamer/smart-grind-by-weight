#include "portal_ui_controller.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>

#include "../../config/constants.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../hardware/display_manager.h"
#include "../../network/portal_command_gate.h"
#include "../../network/wifi_manager.h"
#include "../../system/menu_setting_models.h"
#include "../../system/menu_setting_table.h"
#include "../../system/nvs_settings_store.h"
#include "../../system/statistics_manager.h"
#include "../../system/update_guard.h"
#include "../ui_manager.h"
#include "autotune_controller.h"

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

// The pure snapshot module maps DiagnosticCode values to warning codes without including the enum
#define PORTAL_ASSERT_WARNING(index, code)                                                    \
    static_assert(PORTAL_WARNING_DIAG_CODES[index] == static_cast<int>(DiagnosticCode::code), \
                  "DiagnosticCode changed: update PORTAL_WARNING_DIAG_CODES in network/portal_snapshot.h")
PORTAL_ASSERT_WARNING(0, BOARD_UNRECOGNIZED);
PORTAL_ASSERT_WARNING(1, MOTOR_FAULT);
PORTAL_ASSERT_WARNING(2, MOTOR_DISABLED);
PORTAL_ASSERT_WARNING(3, HX711_NOT_CONNECTED);
PORTAL_ASSERT_WARNING(4, HX711_NO_DATA);
PORTAL_ASSERT_WARNING(5, HX711_SAMPLE_RATE_INVALID);
PORTAL_ASSERT_WARNING(6, MECHANICAL_INSTABILITY);
PORTAL_ASSERT_WARNING(7, LOAD_CELL_NOISY_SUSTAINED);
PORTAL_ASSERT_WARNING(8, LOAD_CELL_NOT_CALIBRATED);
#undef PORTAL_ASSERT_WARNING

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
        stop_seen_ = portal_command_gate.stop_seq();  // a stop from before the UI existed is not ours to replay
        portal_command_gate.attach_ui(true);
    }

    serve_mailbox();
    run_lead_in();  // before the web gate below: a lead-in that began runs to its end even if the switch goes off

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
    const bool state_changed = state != last_state_;
    last_state_ = state;
    if (just_enabled || left_menu || publish_due_ ||
        static_cast<uint32_t>(now - last_publish_ms_) >= PORTAL_STATUS_PUBLISH_MS) {
        publish_snapshot(now, just_enabled || left_menu);
    }
    // The lifetime statistics change at the end of a grind or a motor test; a screen state change is the cue to look
    if (just_enabled || state_changed || static_cast<uint32_t>(now - last_stats_ms_) >= PORTAL_STATS_REFRESH_MS) {
        portal_command_gate.publish_stats(gather_stats());
        last_stats_ms_ = now;
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
    in.diag_mask = ui_manager_->diagnostics_controller_ ? ui_manager_->diagnostics_controller_->active_mask() : 0;
    return in;
}

// get_snapshot() takes the statistics mutex once (also taken by the file-IO task while it saves), so this runs on the
// UI task only
PortalStats PortalUIController::gather_stats() const {
    return portal_stats_from_counters(statistics_manager.get_snapshot());
}

PortalDecisionInput PortalUIController::gather_decision() const {
    PortalDecisionInput in;
    in.ui = portal_ui_mode(ui_manager_->state_machine->get_current_state(),
                           BlockingOperationOverlay::getInstance().is_operation_active());
    in.busy = grinder_busy();
    in.update_held = UpdateGuard::is_held();
    // The mailbox itself admits one request at a time; a remote start in its lead-in still counts as in flight
    in.command_in_flight = lead_in_.active;
    auto* grind = ui_manager_->grind_controller;
    in.start_block = grind ? grind->start_block() : GrindStartBlock::MOTOR_DISABLED;
    in.motor_test_block = ui_manager_->motor_pulse_block();
    return in;
}

//------------------------------------------------------------------------------
// Mailbox: PATCH /api/settings
//------------------------------------------------------------------------------
void PortalUIController::serve_mailbox() {
    // A stop first: it is about the motor, whatever else is queued
    if (portal_stop_pending(portal_command_gate.stop_seq(), &stop_seen_)) handle_stop();

    PortalPatch patch;
    PortalCommandRequest command;
    if (portal_command_gate.take_patch(&patch)) {
        handle_patch(patch);
    } else if (portal_command_gate.take_command(&command)) {
        handle_command(command);
    }
}

// The motor and the grind are already stopped by portal_stop_now() (HTTP task) and the grind control task; this
// is the part only the UI task can do: the autotune run is driven from here, cancel it like its CANCEL button.
void PortalUIController::handle_stop() {
    portal_lead_in_cancel(&lead_in_);  // a stop also cancels a remote start that has not begun yet (no-op otherwise)
    auto* hardware = ui_manager_->hardware_manager;
    auto* autotune = hardware ? hardware->get_autotune_controller() : nullptr;
    if (autotune && autotune->is_active() && ui_manager_->autotune_controller_) {
        LOG_BLE("[PORTAL] stop: cancelling autotune\n");
        ui_manager_->autotune_controller_->handle_cancel();
    }
}

void PortalUIController::handle_command(const PortalCommandRequest& command) {
    PortalMailboxResult result;
    result.error = portal_command_decide(command.cmd, gather_decision());
    if (result.error == PortalError::NONE) {
        switch (command.cmd) {
            case PortalCommand::TARE:
                if (!run_tare()) result.error = PortalError::NOT_READY;
                break;
            case PortalCommand::START: begin_remote_start(command.profile); break;
            case PortalCommand::MOTOR_TEST: result.error = run_motor_test(); break;
            // stop never gets here (it is not a mailbox request)
            default: result.error = PortalError::UNKNOWN_COMMAND; break;
        }
    }
    portal_command_gate.complete(result);
    LOG_BLE("[PORTAL] command %s: %s\n", portal_command_name(command.cmd), portal_error_info(result.error).code);
}

// Same as the Scale page's TARE button: the TARING overlay, then the blocking tare on the next LVGL timer tick. The
// overlay is up from here on, so any other command or write is refused (not_ready) until it is done. 200 means the
// tare was started.
bool PortalUIController::run_tare() {
    auto* hardware = ui_manager_->hardware_manager;
    if (!hardware) return false;
    UIManager* ui = ui_manager_;
    UIOperations::execute_tare(hardware, [ui]() { ui->refresh_auto_action_settings(); });
    return true;
}

// Same run as Menu -> Motor Test after its RUN tap: the pulse, the MOTOR RUNNING overlay with STOP, the poll timer and
// finish_motor_test() (stats, background indicator). No confirm dialog and no lead-in: the request is the confirmation
// (docs/PORTAL-API.md). motor_pulse_block() is checked again inside, immediately before the pulse. 202 means the pulse
// is running (a start that fails after that check is reported as busy).
PortalError PortalUIController::run_motor_test() {
    if (!ui_manager_->menu_controller_) return PortalError::MOTOR_DISABLED;
    return portal_error_from_motor_test_block(ui_manager_->menu_controller_->begin_motor_test());
}

// Remote start, step 1: the command was decided (READY, not busy, no update, start_block NONE). Show the
// touch-to-cancel overlay and answer 202; the grind itself starts when the lead-in ends (run_lead_in).
void PortalUIController::begin_remote_start(int profile) {
    lead_in_profile_ = profile;
    portal_lead_in_begin(&lead_in_, millis());
    lead_in_shown_s_ = portal_lead_in_remaining_s(lead_in_, millis());
    char text[64];
    lead_in_text(text, sizeof(text), lead_in_shown_s_);
    if (ui_manager_->screen_timeout_controller_) ui_manager_->screen_timeout_controller_->hold_awake_remote_start(true);
    PortalLeadIn* lead_in = &lead_in_;  // the overlay's STOP button cancels like a touch anywhere
    BlockingOperationOverlay::getInstance().show_with_stop(text, [lead_in]() { portal_lead_in_cancel(lead_in); });
    LOG_BLE("[PORTAL] remote start: profile %d (%s), grind starts in %u ms unless cancelled\n", profile,
            portal_profile_name(profile), static_cast<unsigned>(PORTAL_LEAD_IN_MS));
}

void PortalUIController::lead_in_text(char* out, size_t cap, uint32_t remaining_s) const {
    const char* name = ui_manager_->profile_controller ? ui_manager_->profile_controller->get_profile_name(lead_in_profile_)
                                                       : portal_profile_name(lead_in_profile_);
    snprintf(out, cap, "Remote start: %s in %u s, touch to cancel", name, static_cast<unsigned>(remaining_s));
}

void PortalUIController::end_lead_in() {
    if (ui_manager_->screen_timeout_controller_) ui_manager_->screen_timeout_controller_->hold_awake_remote_start(false);
    BlockingOperationOverlay::getInstance().hide();
    lead_in_ = PortalLeadIn();
}

// Remote start, step 2: counts the lead-in down and, when it ends, decides again on the current state and starts
// the grind through the same path as the START button.
void PortalUIController::run_lead_in() {
    if (!lead_in_.active) return;
    const uint32_t now = millis();

    // Any touch since the lead-in began cancels (the overlay swallows the touch, so the screen cannot be used)
    auto* hardware = ui_manager_->hardware_manager;
    auto* display = hardware ? hardware->get_display() : nullptr;
    auto* touch = display ? display->get_touch_driver() : nullptr;
    if (!touch) {
        // Nobody could cancel by touch: fail safe, the start is dropped (logged once, the lead-in ends below)
        LOG_BLE("[PORTAL] remote start cancelled: no touch driver\n");
        portal_lead_in_cancel(&lead_in_);
    } else if (portal_lead_in_touched(lead_in_, now, touch->get_ms_since_last_touch())) {
        portal_lead_in_cancel(&lead_in_);
    }

    const PortalLeadInState state = portal_lead_in_poll(&lead_in_, now);  // clears lead_in_ on CANCELLED and GO
    if (state == PortalLeadInState::CANCELLED) {
        end_lead_in();
        LOG_BLE("[PORTAL] remote start cancelled\n");
        return;
    }

    if (state == PortalLeadInState::WAITING) {
        // Something else took over the grinder meanwhile (auto start, a firmware update, a load cell fault): give up
        // now. The overlay is ours, so it is left out of the screen classification.
        PortalDecisionInput in = gather_decision();
        in.command_in_flight = false;
        in.ui = portal_ui_mode(ui_manager_->state_machine->get_current_state(), false);
        const PortalError refused = portal_command_decide(PortalCommand::START, in);
        if (refused != PortalError::NONE) {
            end_lead_in();
            LOG_BLE("[PORTAL] remote start dropped: %s\n", portal_error_info(refused).code);
            return;
        }
        const uint32_t remaining_s = portal_lead_in_remaining_s(lead_in_, now);
        if (remaining_s != lead_in_shown_s_) {
            lead_in_shown_s_ = remaining_s;
            char text[64];
            lead_in_text(text, sizeof(text), remaining_s);
            BlockingOperationOverlay::getInstance().set_message(text);
        }
        return;
    }

    if (state != PortalLeadInState::GO) return;

    // A stop that came in after the last frame's check but before the grind starts still wins
    if (portal_stop_pending(portal_command_gate.stop_seq(), &stop_seen_)) {
        handle_stop();
        end_lead_in();
        LOG_BLE("[PORTAL] remote start cancelled by stop\n");
        return;
    }

    // Time is up: the overlay goes first (it classifies the screen as busy), then the decision is made on what
    // is true right now, exactly as the START button would
    end_lead_in();
    const PortalDecisionInput in = gather_decision();
    const PortalError refused = portal_command_decide(PortalCommand::START, in);
    if (refused != PortalError::NONE) {
        LOG_BLE("[PORTAL] remote start refused: %s\n", portal_error_info(refused).code);
        if (in.ui == PortalUiMode::READY && in.start_block != GrindStartBlock::NONE) {
            ui_manager_->show_start_blocked_notice(in.start_block);
        }
        return;
    }
    if (!ui_manager_->grinding_controller_) return;
    LOG_BLE("[PORTAL] remote start: starting grind, profile %d\n", lead_in_profile_);
    const GrindStartBlock block = ui_manager_->grinding_controller_->start_profile_grind(lead_in_profile_);
    if (block != GrindStartBlock::NONE) ui_manager_->show_start_blocked_notice(block);
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
