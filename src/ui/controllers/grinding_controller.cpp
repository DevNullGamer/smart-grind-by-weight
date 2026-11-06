#include "grinding_controller.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../config/constants.h"
#include "../../controllers/grind_events.h"
#include "../../controllers/grind_mode.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../logging/grind_logging.h"
#include "../../system/nvs_settings_store.h"
#include "../ui_manager.h"

GrindingUIController* GrindingUIController::instance_ = nullptr;

GrindingUIController::GrindingUIController(UIManager* manager)
    : ui_manager_(manager) {
    instance_ = this;
}

void GrindingUIController::build_controls() {
    if (!ui_manager_) {
        return;
    }

    grind_button_ = lv_button_create(lv_screen_active());
    lv_obj_set_size(grind_button_, 100, 100);
    lv_obj_align(grind_button_, LV_ALIGN_BOTTOM_MID, -60, -10);
    lv_obj_set_style_radius(grind_button_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(grind_button_, lv_color_hex(THEME_COLOR_PRIMARY), 0);
    lv_obj_set_style_border_width(grind_button_, 0, 0);
    lv_obj_set_style_shadow_width(grind_button_, 0, 0);

    grind_icon_ = lv_image_create(grind_button_);
    lv_image_set_src(grind_icon_, LV_SYMBOL_PLAY);
    lv_obj_center(grind_icon_);
    lv_obj_set_style_text_font(grind_icon_, &lv_font_montserrat_24, 0);

    pulse_button_ = lv_button_create(lv_screen_active());
    lv_obj_set_size(pulse_button_, 100, 100);
    lv_obj_align(pulse_button_, LV_ALIGN_BOTTOM_MID, 60, -10);
    lv_obj_set_style_radius(pulse_button_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pulse_button_, lv_color_hex(THEME_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(pulse_button_, 0, 0);
    lv_obj_set_style_shadow_width(pulse_button_, 0, 0);

    pulse_icon_ = lv_image_create(pulse_button_);
    lv_image_set_src(pulse_icon_, LV_SYMBOL_PLUS);
    lv_obj_center(pulse_icon_);
    lv_obj_set_style_text_font(pulse_icon_, &lv_font_montserrat_32, 0);

    lv_obj_set_hidden(pulse_button_, true);
}

void GrindingUIController::register_events() {
    if (!ui_manager_) {
        return;
    }

    if (grind_button_) {
        lv_obj_add_event_cb(grind_button_, [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            auto* controller = static_cast<GrindingUIController*>(lv_event_get_user_data(e));
            if (controller) {
                controller->handle_grind_button();
            }
        }, LV_EVENT_CLICKED, this);
    }

    if (pulse_button_) {
        lv_obj_add_event_cb(pulse_button_, [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            auto* controller = static_cast<GrindingUIController*>(lv_event_get_user_data(e));
            if (controller) {
                controller->handle_pulse_button();
            }
        }, LV_EVENT_CLICKED, this);
    }

    if (lv_obj_t* arc = ui_manager_->grinding_screen.get_arc_screen_obj()) {
        lv_obj_add_event_cb(arc, [](lv_event_t* e) {
            if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                if (auto* controller = static_cast<GrindingUIController*>(lv_event_get_user_data(e))) {
                    controller->handle_layout_toggle();
                }
            }
        }, LV_EVENT_CLICKED, this);
    }

    if (lv_obj_t* chart = ui_manager_->grinding_screen.get_chart_screen_obj()) {
        lv_obj_add_event_cb(chart, [](lv_event_t* e) {
            if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                if (auto* controller = static_cast<GrindingUIController*>(lv_event_get_user_data(e))) {
                    controller->handle_layout_toggle();
                }
            }
        }, LV_EVENT_CLICKED, this);
    }

    // NOTE: Purge confirm reuses existing grind_button_ (CANCEL) and pulse_button_ (CONTINUE)
    // No additional event registration needed - handle_pulse_button() checks for PURGE_CONFIRM phase
}

void GrindingUIController::on_state_changed(UIState new_state) {
    if (!ui_manager_) {
        return;
    }

    if (new_state != UIState::GRIND_COMPLETE && grind_complete_timer_) {
        lv_timer_delete(grind_complete_timer_);
        grind_complete_timer_ = nullptr;
    }
    if (new_state != UIState::GRIND_TIMEOUT && grind_timeout_timer_) {
        lv_timer_delete(grind_timeout_timer_);
        grind_timeout_timer_ = nullptr;
    }

    switch (new_state) {
        case UIState::READY:
            enter_ready_state();
            break;
        case UIState::EDIT:
            enter_edit_state();
            break;
        case UIState::GRINDING:
            enter_grinding_state();
            break;
        case UIState::GRIND_COMPLETE:
            enter_grind_complete_state();
            break;
        case UIState::GRIND_TIMEOUT:
            enter_grind_timeout_state();
            break;
        case UIState::MENU:
        case UIState::CALIBRATION:
        case UIState::CONFIRM:
        case UIState::OTA_UPDATE:
        case UIState::OTA_UPDATE_FAILED:
            enter_menu_state();
            break;
        default:
            break;
    }

    update_grind_button_icon();
}

void GrindingUIController::update(UIState current_state) {
    if (!ui_manager_) {
        return;
    }

    switch (current_state) {
        case UIState::GRIND_COMPLETE: {
            WeightSensor* weight_sensor = ui_manager_->hardware_manager->get_weight_sensor();
            if (weight_sensor) {
                float current_weight = weight_sensor->get_display_weight();
                ui_manager_->grinding_screen.update_current_weight(current_weight);
            }
            ui_manager_->grinding_screen.update_progress(final_grind_progress_);
            break;
        }
        case UIState::GRIND_TIMEOUT: {
            ui_manager_->grinding_screen.update_current_weight(error_grind_weight_);
            ui_manager_->grinding_screen.update_progress(error_grind_progress_);
            char error_display[64];
            const char* message = error_message_[0] ? error_message_ : "Error";
            std::snprintf(error_display, sizeof(error_display), "%s", message);
            ui_manager_->grinding_screen.update_target_weight_text(error_display);
            break;
        }
        default:
            break;
    }
}

void GrindingUIController::handle_grind_button() {
    if (!ui_manager_ || !ui_manager_->state_machine) {
        return;
    }

    LOG_BLE("[%lums BUTTON_PRESS] Grind button pressed in state: %s\n",
            millis(), ui_manager_->state_machine->is_state(UIState::READY) ? "READY" :
                       ui_manager_->state_machine->is_state(UIState::GRINDING) ? "GRINDING" :
                       ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE) ? "GRIND_COMPLETE" :
                       ui_manager_->state_machine->is_state(UIState::GRIND_TIMEOUT) ? "GRIND_TIMEOUT" :
                       ui_manager_->state_machine->is_state(UIState::PURGE_CONFIRM) ? "PURGE_CONFIRM" : "OTHER");

    if (ui_manager_->state_machine->is_state(UIState::PURGE_CONFIRM)) {
        // Cancel grind during purge confirmation
        if (ui_manager_->grind_controller) {
            ui_manager_->grind_controller->stop_grind();
        }
    } else if (ui_manager_->state_machine->is_state(UIState::READY)) {
        if (ui_manager_->current_tab == ui_manager_->menu_tab_index()) {
            ui_manager_->switch_to_state(UIState::MENU);
            return;
        }

        if (ui_manager_->profile_controller) {
            const GrindStartBlock start_block =
                start_profile_grind(ui_manager_->profile_controller->get_current_profile());
            if (start_block != GrindStartBlock::NONE) {
                ui_manager_->show_start_blocked_notice(start_block);  // The controller already logged the refusal
            }
        }
    } else if (ui_manager_->state_machine->is_state(UIState::GRINDING)) {
        if (ui_manager_->grind_controller) {
            ui_manager_->grind_controller->stop_grind();
        }
    } else if (ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE) ||
               ui_manager_->state_machine->is_state(UIState::GRIND_TIMEOUT)) {
        if (ui_manager_->grind_controller) {
            ui_manager_->grind_controller->return_to_idle();
        }
    }
}

// The start of the START button (READY screen), also used by the web panel's remote start: the stored target of the
// profile in the current grind mode. Does not change the profile selected on the screen.
GrindStartBlock GrindingUIController::start_profile_grind(int profile) {
    if (!ui_manager_ || !ui_manager_->grind_controller || !ui_manager_->profile_controller) {
        return GrindStartBlock::MOTOR_DISABLED;
    }

    const float target_weight = ui_manager_->profile_controller->get_profile_weight(profile);
    const float target_time_seconds = ui_manager_->profile_controller->get_profile_time(profile);
    const uint32_t target_time_ms = static_cast<uint32_t>((target_time_seconds * 1000.0f) + 0.5f);
    return begin_grind(static_cast<uint8_t>(profile), target_weight, target_time_ms, ui_manager_->current_mode);
}

// GLink start: weight-mode grind of the stored weight of `profile` (0..3) through the same body. The grinder's grind
// mode setting is not touched: the session runs in WEIGHT mode and the screen follows the controller's events
// (restoring the user's mode on return to idle is the GLink controller's job). The history clamps the profile to 0..2,
// so the Profile Dose (3) is recorded as CUSTOM.
GrindStartBlock GrindingUIController::start_glink_grind(int profile) {
    // Outside the profile range or the dose range (or no controllers) there is no matching GrindStartBlock:
    // MOTOR_DISABLED is returned as the generic refusal. The caller checks the range first and must not show the
    // motor dialog for it.
    if (!ui_manager_ || !ui_manager_->grind_controller || !ui_manager_->profile_controller || profile < 0 ||
        profile >= GLINK_PROFILE_COUNT_MAX) {
        return GrindStartBlock::MOTOR_DISABLED;
    }
    const float grams = ui_manager_->profile_controller->get_profile_weight(profile);
    if (!std::isfinite(grams) || grams < GLINK_DOSE_MIN_G || grams > GLINK_DOSE_MAX_G) {
        return GrindStartBlock::MOTOR_DISABLED;
    }
    const GrindStartBlock block = begin_grind(static_cast<uint8_t>(profile), grams, 0, GrindMode::WEIGHT);
    // The session runs in WEIGHT mode whatever the user's grind mode is; the grind events move UIManager::current_mode
    // and the grinding screen along, so enter_ready_state() puts the stored mode back
    if (block == GrindStartBlock::NONE) forced_mode_.arm();
    return block;
}

GrindStartBlock GrindingUIController::begin_grind(uint8_t profile_id, float target_weight, uint32_t target_time_ms,
                                                  GrindMode mode) {
    ui_manager_->grind_controller->set_grind_profile_id(profile_id);

    LOG_BLE("[%lums GRIND_START] About to call start_grind()\n", millis());
    error_message_[0] = '\0';
    error_grind_weight_ = 0.0f;
    error_grind_progress_ = 0;

    const GrindStartBlock start_block =
        ui_manager_->grind_controller->start_grind(target_weight, target_time_ms, mode);
    LOG_BLE("[%lums GRIND_START] start_grind() returned\n", millis());
    return start_block;
}

void GrindingUIController::handle_pulse_button() {
    if (!ui_manager_ || !ui_manager_->grind_controller) {
        return;
    }

    // Check if we're in PURGE_CONFIRM phase - pulse button acts as CONTINUE
    if (ui_manager_->purge_confirm_screen.is_visible()) {
        handle_purge_confirm_continue();
        return;
    }

    // Normal time mode pulse behavior
    if (ui_manager_->grind_controller->can_pulse()) {
        LOG_BLE("[UIManager] Pulse button clicked - requesting additional pulse\n");
        ui_manager_->grind_controller->start_additional_pulse();
        reset_grind_complete_timer();
    } else {
        LOG_BLE("[UIManager] Pulse button clicked but pulsing not allowed\n");
    }
}

void GrindingUIController::handle_layout_toggle() {
    if (!ui_manager_ || !ui_manager_->state_machine) {
        return;
    }

    if (ui_manager_->state_machine->is_state(UIState::GRINDING) ||
        ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE) ||
        ui_manager_->state_machine->is_state(UIState::GRIND_TIMEOUT)) {
        GrindScreenLayout current_layout = ui_manager_->grinding_screen.get_layout();
        if (current_layout == GrindScreenLayout::MINIMAL_ARC) {
            ui_manager_->grinding_screen.set_layout(GrindScreenLayout::NERDY_CHART);
        } else {
            ui_manager_->grinding_screen.set_layout(GrindScreenLayout::MINIMAL_ARC);
        }
    }
}

void GrindingUIController::handle_purge_confirm_continue() {
    if (!ui_manager_ || !ui_manager_->grind_controller) {
        return;
    }

    // Check if "Keep purge grinds from now on" checkbox is checked
    if (ui_manager_->purge_confirm_screen.is_checkbox_checked()) {
        LOG_BLE("[%lums PURGE] User chose to keep grinds - switching to Prime mode\n", millis());

        // Switch grinder purge mode from Purge to Prime in preferences
        ui_settings.set_purge_mode(static_cast<int>(GrinderPurgeMode::PRIME));
    }

    // Hide the purge confirmation screen and continue grinding
    ui_manager_->purge_confirm_screen.hide();
    ui_manager_->switch_to_state(UIState::GRINDING);

    // Tell the grind controller to continue from PURGE_CONFIRM to PREDICTIVE
    ui_manager_->grind_controller->continue_from_purge();
}

void GrindingUIController::update_grind_button_icon() {
    if (!ui_manager_ || !grind_button_ || !grind_icon_) {
        return;
    }

    if (ui_manager_->state_machine->is_state(UIState::PURGE_CONFIRM)) {
        // During purge confirm, show STOP icon (user can cancel the grind)
        lv_image_set_src(grind_icon_, LV_SYMBOL_STOP);
        lv_obj_set_style_bg_color(grind_button_, lv_color_hex(THEME_COLOR_ERROR), 0);
    } else if (ui_manager_->state_machine->is_state(UIState::GRINDING)) {
        lv_image_set_src(grind_icon_, LV_SYMBOL_STOP);
        lv_obj_set_style_bg_color(grind_button_,
                                  ui_manager_->current_mode == GrindMode::TIME
                                      ? lv_color_hex(THEME_COLOR_ACCENT)
                                      : lv_color_hex(THEME_COLOR_PRIMARY),
                                  0);
    } else if (ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE)) {
        lv_image_set_src(grind_icon_, LV_SYMBOL_OK);
        lv_obj_set_style_bg_color(grind_button_, lv_color_hex(THEME_COLOR_SUCCESS), 0);
    } else if (ui_manager_->state_machine->is_state(UIState::GRIND_TIMEOUT)) {
        lv_image_set_src(grind_icon_, LV_SYMBOL_CLOSE);
        lv_obj_set_style_bg_color(grind_button_, lv_color_hex(THEME_COLOR_WARNING), 0);
    } else if (ui_manager_->state_machine->is_state(UIState::READY) &&
               ui_manager_->current_tab == ui_manager_->menu_tab_index()) {
        lv_image_set_src(grind_icon_, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_bg_color(grind_button_, lv_color_hex(THEME_COLOR_NEUTRAL), 0);
    } else {
        lv_image_set_src(grind_icon_, LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(grind_button_,
                                  ui_manager_->current_mode == GrindMode::TIME
                                      ? lv_color_hex(THEME_COLOR_ACCENT)
                                      : lv_color_hex(THEME_COLOR_PRIMARY),
                                  0);
    }

    update_button_layout();
}

void GrindingUIController::update_button_layout() {
    if (!ui_manager_ || !grind_button_) {
        return;
    }

    // Check if we're in PURGE_CONFIRM phase (show dual buttons for CANCEL + CONTINUE)
    bool in_purge_confirm = ui_manager_->purge_confirm_screen.is_visible();

    bool should_show_pulse = (ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE) &&
                              ui_manager_->current_mode == GrindMode::TIME);

    if (in_purge_confirm || should_show_pulse) {
        // Dual button layout: left button at -60, right button at +60
        lv_obj_align(grind_button_, LV_ALIGN_BOTTOM_MID, -60, -10);
        if (pulse_button_) {
            lv_obj_align(pulse_button_, LV_ALIGN_BOTTOM_MID, 60, -10);
            lv_obj_set_hidden(pulse_button_, false);

            if (in_purge_confirm) {
                // Purge confirm: pulse button acts as CONTINUE (always enabled)
                lv_image_set_src(pulse_icon_, LV_SYMBOL_OK);
                lv_obj_set_style_bg_color(pulse_button_, lv_color_hex(THEME_COLOR_SUCCESS), 0);
                lv_obj_remove_state(pulse_button_, LV_STATE_DISABLED);
                lv_obj_set_style_bg_opa(pulse_button_, LV_OPA_COVER, 0);
            } else if (ui_manager_->grind_controller && ui_manager_->grind_controller->can_pulse()) {
                // Time mode pulse: enable/disable based on can_pulse()
                lv_image_set_src(pulse_icon_, LV_SYMBOL_PLUS);
                lv_obj_set_style_bg_color(pulse_button_, lv_color_hex(THEME_COLOR_ACCENT), 0);
                lv_obj_remove_state(pulse_button_, LV_STATE_DISABLED);
                lv_obj_set_style_bg_opa(pulse_button_, LV_OPA_COVER, 0);
            } else {
                // Time mode pulse: disabled
                lv_image_set_src(pulse_icon_, LV_SYMBOL_PLUS);
                lv_obj_set_style_bg_color(pulse_button_, lv_color_hex(THEME_COLOR_ACCENT), 0);
                lv_obj_add_state(pulse_button_, LV_STATE_DISABLED);
                lv_obj_set_style_bg_opa(pulse_button_, LV_OPA_50, LV_STATE_DISABLED);
            }
        }
    } else {
        // Single button layout: centered at 0
        lv_obj_align(grind_button_, LV_ALIGN_BOTTOM_MID, 0, -10);
        if (pulse_button_) {
            lv_obj_set_hidden(pulse_button_, true);
        }
    }
}

void GrindingUIController::update_grinding_targets() {
    if (!ui_manager_ || !ui_manager_->grind_controller || !ui_manager_->profile_controller) {
        return;
    }

    const auto& session = ui_manager_->grind_controller->get_session_descriptor();
    ui_manager_->grinding_screen.set_chart_time_prediction(session.target_time_ms);
    ui_manager_->grinding_screen.update_target_weight(session.target_weight);
    if (session.mode == GrindMode::TIME && session.target_time_ms > 0) {
        float target_time_seconds = static_cast<float>(session.target_time_ms) / 1000.0f;
        ui_manager_->grinding_screen.update_target_time(target_time_seconds);
    }
}

void GrindingUIController::reset_grind_complete_timer() {
    if (!ui_manager_ || !ui_manager_->state_machine) {
        return;
    }

    if (ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE) && grind_complete_timer_) {
        lv_timer_delete(grind_complete_timer_);
        grind_complete_timer_ = nullptr;
        start_grind_complete_timer();
    }
}

void GrindingUIController::handle_grind_event(const GrindEventData& event_data) {
    if (!ui_manager_ || !ui_manager_->state_machine) {
        return;
    }

    switch (event_data.event) {
        case UIGrindEvent::PHASE_CHANGED: {
            ui_manager_->current_mode = event_data.mode;

            // Handle PURGE_CONFIRM phase specially - show purge confirmation popup
            if (event_data.phase == GrindPhase::PURGE_CONFIRM) {
                LOG_UI_DEBUG("[%lums UI_TRANSITION] Switching to PURGE_CONFIRM state\n", millis());
                ui_manager_->switch_to_state(UIState::PURGE_CONFIRM);
                update_grind_button_icon();  // Update button icon to STOP and reposition for dual-button layout
            } else if (event_data.phase != GrindPhase::IDLE &&
                       event_data.phase != GrindPhase::TIME_ADDITIONAL_PULSE &&
                       !ui_manager_->state_machine->is_state(UIState::GRINDING)) {
                LOG_UI_DEBUG("[%lums UI_TRANSITION] Switching to GRINDING state due to phase: %s\n",
                             millis(), event_data.phase_display_text);
                WeightSensor* weight_sensor = ui_manager_->hardware_manager->get_weight_sensor();
                ui_manager_->grinding_screen.update_profile_name(session_profile_name());
                ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
                chart_updates_enabled_ = true;
                update_grinding_targets();
                if (weight_sensor) {
                    ui_manager_->grinding_screen.update_current_weight(weight_sensor->get_display_weight());
                }
                ui_manager_->grinding_screen.update_progress(0);
                ui_manager_->switch_to_state(UIState::GRINDING);

                if (event_data.phase == GrindPhase::INITIALIZING) {
                    ui_manager_->grind_controller->ui_acknowledge_phase_transition();
                    LOG_UI_DEBUG("[%lums UI_ACKNOWLEDGMENT] INITIALIZING phase confirmed, ready for SETUP\n", millis());
                }
            }

            if (ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE)) {
                update_button_layout();
            }

            if (event_data.show_taring_text) {
                ui_manager_->grinding_screen.update_tare_display();
            } else {
                ui_manager_->grinding_screen.update_current_weight(event_data.current_weight);
                ui_manager_->grinding_screen.update_progress(event_data.progress_percent);

                if (chart_updates_enabled_ &&
                    event_data.phase != GrindPhase::IDLE && event_data.phase != GrindPhase::TARING &&
                    event_data.phase != GrindPhase::TARE_CONFIRM && event_data.phase != GrindPhase::INITIALIZING &&
                    event_data.phase != GrindPhase::SETUP && event_data.phase != GrindPhase::COMPLETED &&
                    event_data.phase != GrindPhase::TIMEOUT && event_data.phase != GrindPhase::TIME_ADDITIONAL_PULSE &&
                    event_data.phase != GrindPhase::PURGE_CONFIRM) {
                    ui_manager_->grinding_screen.add_chart_data_point(event_data.current_weight, event_data.flow_rate, millis());
                }
            }
            break;
        }
        case UIGrindEvent::PROGRESS_UPDATED: {
            if (event_data.show_taring_text) {
                ui_manager_->grinding_screen.update_tare_display();
            } else {
                ui_manager_->current_mode = event_data.mode;
                ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
                ui_manager_->grinding_screen.update_current_weight(event_data.current_weight);
                ui_manager_->grinding_screen.update_progress(event_data.progress_percent);

                if (chart_updates_enabled_ &&
                    event_data.phase != GrindPhase::IDLE && event_data.phase != GrindPhase::TARING &&
                    event_data.phase != GrindPhase::TARE_CONFIRM && event_data.phase != GrindPhase::INITIALIZING &&
                    event_data.phase != GrindPhase::SETUP && event_data.phase != GrindPhase::COMPLETED &&
                    event_data.phase != GrindPhase::TIMEOUT && event_data.phase != GrindPhase::TIME_ADDITIONAL_PULSE &&
                    event_data.phase != GrindPhase::PURGE_CONFIRM) {
                    ui_manager_->grinding_screen.add_chart_data_point(event_data.current_weight, event_data.flow_rate, millis());
                }
            }
            break;
        }
        case UIGrindEvent::COMPLETED: {
            ui_manager_->current_mode = event_data.mode;
            ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
            final_grind_weight_ = event_data.final_weight;
            final_grind_progress_ = event_data.progress_percent;
            LOG_BLE("GRIND COMPLETE - Final settled weight captured: %.2fg (Progress: %d%%)\n",
                    final_grind_weight_, final_grind_progress_);
            chart_updates_enabled_ = false;
            ui_manager_->switch_to_state(UIState::GRIND_COMPLETE);
            start_grind_complete_timer();
            break;
        }
        case UIGrindEvent::TIMEOUT: {
            ui_manager_->current_mode = event_data.mode;
            ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
            error_grind_weight_ = event_data.error_weight;
            error_grind_progress_ = event_data.error_progress;
            const char* message = event_data.error_message ? event_data.error_message : "Error";
            std::strncpy(error_message_, message, sizeof(error_message_) - 1);
            error_message_[sizeof(error_message_) - 1] = '\0';
            LOG_BLE("GRIND ERROR - %s, Weight: %.2fg (Progress: %d%%)\n",
                    error_message_, error_grind_weight_, error_grind_progress_);
            chart_updates_enabled_ = false;
            ui_manager_->switch_to_state(UIState::GRIND_TIMEOUT);
            start_grind_timeout_timer();
            break;
        }
        case UIGrindEvent::STOPPED: {
            cancel_timers();
            chart_updates_enabled_ = false;
            ui_manager_->switch_to_state(UIState::READY);
            break;
        }
        case UIGrindEvent::BACKGROUND_CHANGE:
#if DEBUG_ENABLE_GRINDER_BACKGROUND_INDICATOR
        {
            ui_manager_->set_background_active(event_data.background_active);
            LOG_UI_DEBUG("[UIManager] Background: %s\n", event_data.background_active ? "ACTIVE" : "INACTIVE");
        }
#endif
            break;
        case UIGrindEvent::PULSE_AVAILABLE:
            LOG_BLE("[UIManager] Pulse available - updating button layout\n");
            update_button_layout();
            break;
        case UIGrindEvent::PULSE_STARTED:
            LOG_BLE("[UIManager] Pulse #%d started (%.1fms)\n",
                    event_data.pulse_count, (float)event_data.pulse_duration_ms);
#if DEBUG_ENABLE_GRINDER_BACKGROUND_INDICATOR
        ui_manager_->set_background_active(true);
#endif
            break;
        case UIGrindEvent::PULSE_COMPLETED:
            LOG_BLE("[UIManager] Pulse #%d completed - weight: %.2fg\n",
                    event_data.pulse_count, event_data.current_weight);
            ui_manager_->grinding_screen.update_current_weight(event_data.current_weight);
            update_button_layout();
            break;
        default:
            break;
    }
}

void GrindingUIController::dispatch_event(const GrindEventData& event_data) {
    if (instance_) {
        instance_->handle_grind_event(event_data);
    }
}

void GrindingUIController::enter_ready_state() {
    // After a GLink grind (STOPPED, COMPLETED or TIMEOUT all end here): the user's grind mode, not the forced WEIGHT one
    GrindMode restored_mode;
    if (ui_manager_->profile_controller && forced_mode_.on_ready(ui_manager_->profile_controller->get_grind_mode(), &restored_mode)) {
        ui_manager_->current_mode = restored_mode;
        ui_manager_->edit_target = get_current_profile_target(*ui_manager_->profile_controller, ui_manager_->current_mode);
        if (ui_manager_->ready_controller_) ui_manager_->ready_controller_->refresh_profiles();
    }
    if (!grind_button_) {
        return;
    }

    lv_obj_set_hidden(grind_button_, false);
    if (pulse_button_) {
        lv_obj_set_hidden(pulse_button_, true);
    }
    ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
}

void GrindingUIController::enter_edit_state() {
    if (grind_button_) {
        lv_obj_set_hidden(grind_button_, true);
    }
    if (pulse_button_) {
        lv_obj_set_hidden(pulse_button_, true);
    }
}

// Name of the profile the grind session was started with (a remote start may use one that is not selected on READY;
// for a touch start it is the selected one)
const char* GrindingUIController::session_profile_name() const {
    const uint8_t profile_id = ui_manager_->grind_controller ? ui_manager_->grind_controller->get_session_descriptor().profile_id
                                                             : ui_manager_->profile_controller->get_current_profile();
    return ui_manager_->profile_controller->get_profile_name(profile_id);
}

void GrindingUIController::enter_grinding_state() {
    WeightSensor* weight_sensor = ui_manager_->hardware_manager->get_weight_sensor();
    ui_manager_->grinding_screen.reset_chart_data();
    ui_manager_->grinding_screen.update_profile_name(session_profile_name());
    ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
    chart_updates_enabled_ = true;
    update_grinding_targets();
    if (weight_sensor) {
        ui_manager_->grinding_screen.update_current_weight(weight_sensor->get_display_weight());
    }
    ui_manager_->grinding_screen.update_progress(0);
    if (grind_button_) {
        lv_obj_set_hidden(grind_button_, false);
    }
}

void GrindingUIController::enter_grind_complete_state() {
    if (grind_button_) {
        lv_obj_set_hidden(grind_button_, false);
    }
    ui_manager_->grinding_screen.update_profile_name(session_profile_name());
    ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
    ui_manager_->grinding_screen.update_current_weight(final_grind_weight_);
    ui_manager_->grinding_screen.update_progress(final_grind_progress_);
}

void GrindingUIController::enter_grind_timeout_state() {
    if (grind_button_) {
        lv_obj_set_hidden(grind_button_, false);
    }
    ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
    ui_manager_->grinding_screen.update_profile_name("ERROR");
    char error_display[64];
    const char* message = error_message_[0] ? error_message_ : "Error";
    std::snprintf(error_display, sizeof(error_display), "%s", message);
    ui_manager_->grinding_screen.update_target_weight_text(error_display);
    ui_manager_->grinding_screen.update_current_weight(error_grind_weight_);
    ui_manager_->grinding_screen.update_progress(error_grind_progress_);
}

void GrindingUIController::enter_menu_state() {
    if (grind_button_) {
        lv_obj_set_hidden(grind_button_, true);
    }
    if (pulse_button_) {
        lv_obj_set_hidden(pulse_button_, true);
    }
}

void GrindingUIController::start_grind_complete_timer() {
    if (grind_complete_timer_) {
        lv_timer_delete(grind_complete_timer_);
    }
    grind_complete_timer_ = lv_timer_create(grind_complete_timer_cb, 60000, this);
    lv_timer_set_repeat_count(grind_complete_timer_, 1);
}

void GrindingUIController::start_grind_timeout_timer() {
    if (grind_timeout_timer_) {
        lv_timer_delete(grind_timeout_timer_);
    }
    grind_timeout_timer_ = lv_timer_create(grind_timeout_timer_cb, 60000, this);
    lv_timer_set_repeat_count(grind_timeout_timer_, 1);
}

void GrindingUIController::cancel_timers() {
    if (grind_complete_timer_) {
        lv_timer_delete(grind_complete_timer_);
        grind_complete_timer_ = nullptr;
    }
    if (grind_timeout_timer_) {
        lv_timer_delete(grind_timeout_timer_);
        grind_timeout_timer_ = nullptr;
    }
}

void GrindingUIController::grind_complete_timer_cb(lv_timer_t* timer) {
    auto* controller = static_cast<GrindingUIController*>(lv_timer_get_user_data(timer));
    if (!controller || !controller->ui_manager_ || !controller->ui_manager_->grind_controller) {
        return;
    }

    controller->ui_manager_->grind_controller->return_to_idle();
    controller->grind_complete_timer_ = nullptr;
}

void GrindingUIController::grind_timeout_timer_cb(lv_timer_t* timer) {
    auto* controller = static_cast<GrindingUIController*>(lv_timer_get_user_data(timer));
    if (!controller || !controller->ui_manager_ || !controller->ui_manager_->grind_controller) {
        return;
    }

    controller->ui_manager_->grind_controller->return_to_idle();
    controller->grind_timeout_timer_ = nullptr;
}
