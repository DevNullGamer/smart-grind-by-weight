// MenuUIController: table-driven setting toggles, sliders and radio groups (see system/menu_setting_table.h).
#include "menu_controller.h"

#include "../../bluetooth/manager.h"
#include "../../config/constants.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../hardware/board_detect.h"
#include "../../hardware/hardware_manager.h"
#include "../../system/menu_setting_table.h"
#include "../../system/nvs_settings_store.h"
#include "../components/blocking_overlay.h"
#include "../event_bridge_lvgl.h"
#include "../ui_helpers.h"
#include "../ui_manager.h"
#include "../screens/menu_screen.h"

void MenuUIController::register_setting_toggle_events() {
    for (size_t i = 0; i < TOGGLE_SETTING_COUNT; i++) {
        const ToggleSetting id = static_cast<ToggleSetting>(i);
        EventBridgeLVGL::register_handler(MenuScreen::setting_toggle_event(id),
                                          [this, id](lv_event_t*) { handle_setting_toggle(id); });
    }
}

void MenuUIController::handle_setting_toggle(ToggleSetting id) {
    if (!ui_manager_) return;

    auto* toggle = ui_manager_->menu_screen.get_setting_toggle(id);
    if (!toggle) return;

    const ToggleSettingDef& def = toggle_setting_def(id);
    const bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);

    (ui_settings.*def.set)(enabled);

    if (def.after == ToggleAfter::REFRESH_AUTO_ACTIONS) {
        ui_manager_->refresh_auto_action_settings();
    } else if (def.after == ToggleAfter::APPLY_GLINK_LINK) {
        apply_glink_link(enabled);
    }

    LOG_DEBUG_PRINTLN(enabled ? def.log_on : def.log_off);
}

// Starting the link may bring the BLE stack up (about 0.6 s of init delays), so it runs behind the same overlay as
// the Bluetooth Enabled toggle. While Bluetooth is Enabled (FULL) nothing changes until it ends.
void MenuUIController::apply_glink_link(bool enabled) {
    auto* ble = ui_manager_ ? ui_manager_->bluetooth_manager : nullptr;
    if (!ble) return;

    auto operation = [ble, enabled]() { ble->set_glink_link(enabled); };
    auto completion = [this]() { ui_manager_->menu_screen.update_ble_status(); };
    BlockingOperationOverlay::getInstance().show_and_execute(BlockingOperation::BLE_ENABLING, operation, completion);
}

void MenuUIController::register_setting_slider_events() {
    for (size_t i = 0; i < SLIDER_SETTING_COUNT; i++) {
        const SliderSetting id = static_cast<SliderSetting>(i);
        EventBridgeLVGL::register_handler(MenuScreen::setting_slider_event(id, false),
                                          [this, id](lv_event_t*) { handle_setting_slider(id, false); });
        EventBridgeLVGL::register_handler(MenuScreen::setting_slider_event(id, true),
                                          [this, id](lv_event_t*) { handle_setting_slider(id, true); });
    }
}

// Value changed (while dragging): label and live brightness preview only, nothing is stored.
// Released: the same, then the value is stored and the definition's follow-up runs.
void MenuUIController::handle_setting_slider(SliderSetting id, bool released) {
    if (!ui_manager_) return;

    auto* slider = ui_manager_->menu_screen.get_setting_slider(id);
    if (!slider) return;

    const SliderSettingDef& def = slider_setting_def(id);
    const int raw_pos = lv_slider_get_value(slider);
    const int pos = def.clamp_pos(raw_pos);
    if (pos != raw_pos) {
        lv_slider_set_value(slider, pos, LV_ANIM_OFF);
    }

    if (released) {
        def.store(ui_settings, pos);
    }
    ui_manager_->menu_screen.set_setting_slider_label(id, pos);

    if (released && def.after == SliderAfter::RELOAD_SCREEN_TIMEOUTS) {
        ui_manager_->refresh_screen_timeout_settings();
    }

    if (def.live_brightness && !released) {
        const float brightness = def.live_brightness(pos);
        ui_manager_->get_hardware_manager()->get_display()->set_brightness(brightness);
        LOG_DEBUG_PRINTF("%s %d%% (%.2f)\n", def.log_name, pos, brightness);
    } else if (released && def.after == SliderAfter::RESTORE_NORMAL_BRIGHTNESS) {
        const float normal = get_normal_brightness();
        ui_manager_->get_hardware_manager()->get_display()->set_brightness(normal);
        LOG_DEBUG_PRINTF("Touch released - restored normal brightness to %.2f\n", normal);
    } else if (released && !def.live_brightness) {
        LOG_DEBUG_PRINTF("%s %d\n", def.log_name, pos);
    }
}

void MenuUIController::register_setting_radio_events() {
    EventBridgeLVGL::register_handler(MenuScreen::setting_radio_event(RadioSetting::GRIND_MODE),
                                      [this](lv_event_t*) { handle_grind_mode_radio_button(); });
    EventBridgeLVGL::register_handler(MenuScreen::setting_radio_event(RadioSetting::PURGE_MODE),
                                      [this](lv_event_t*) { handle_grinder_purge_mode_radio_button(); });
    EventBridgeLVGL::register_handler(MenuScreen::setting_radio_event(RadioSetting::BOARD_OVERRIDE),
                                      [this](lv_event_t*) { handle_board_override_radio_button(); });
}

void MenuUIController::handle_grind_mode_radio_button() {
    if (!ui_manager_ || !ui_manager_->profile_controller) return;

    lv_obj_t* radio_group = ui_manager_->menu_screen.get_setting_radio(RadioSetting::GRIND_MODE);
    if (!radio_group) return;

    int selected_index = radio_button_group_get_selection(radio_group);
    if (selected_index < 0) return;

    GrindMode new_mode = (selected_index == 0) ? GrindMode::WEIGHT : GrindMode::TIME;
    if (!ui_manager_->profile_controller->set_grind_mode(new_mode)) {
        // Not stored: the profile controller kept the old mode, so the UI does too; show the real selection again
        ui_manager_->menu_screen.refresh_setting_radios();
        return;
    }
    ui_manager_->current_mode = new_mode;
    if (ui_manager_->ready_controller_) {
        ui_manager_->ready_controller_->refresh_profiles();
    }
    ui_manager_->edit_target = get_current_profile_target(*ui_manager_->profile_controller, new_mode);
    if (ui_manager_->state_machine->is_state(UIState::EDIT)) {
        if (ui_manager_->edit_controller_) {
            ui_manager_->edit_controller_->update_display();
        }
    }

    LOG_DEBUG_PRINTLN(selected_index == 0 ? "Grind mode set to WEIGHT via radio button" : "Grind mode set to TIME via radio button");
}

void MenuUIController::handle_grinder_purge_mode_radio_button() {
    if (!ui_manager_) return;

    auto* radio_group = ui_manager_->menu_screen.get_setting_radio(RadioSetting::PURGE_MODE);
    if (!radio_group) return;

    int selected_index = radio_button_group_get_selection(radio_group);

    ui_settings.set_purge_mode(selected_index);

    LOG_DEBUG_PRINTLN(selected_index == 0 ? "Grinder purge mode: Prime (keep coffee)" : "Grinder purge mode: Purge (discard grinds)");
}

void MenuUIController::handle_board_override_radio_button() {
    if (!ui_manager_) return;

    auto* radio_group = ui_manager_->menu_screen.get_setting_radio(RadioSetting::BOARD_OVERRIDE);
    if (!radio_group) return;

    // Radio index matches BoardOverride: 0 = Auto, 1 = V1, 2 = V2
    int selected_index = radio_button_group_get_selection(radio_group);
    if (selected_index < 0 || selected_index > static_cast<int>(BoardOverride::FORCE_V2)) return;

    if (board_override_store(static_cast<BoardOverride>(selected_index))) {
        LOG_BLE("[BOARD] Override set to %d by user (applies after restart)\n", selected_index);
    } else {
        LOG_BLE("[BOARD] ERROR: could not store board override\n");
    }
}
