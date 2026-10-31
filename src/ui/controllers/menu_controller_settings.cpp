// MenuUIController: table-driven setting toggles and sliders (see system/menu_setting_table.h).
#include "menu_controller.h"

#include "../../config/constants.h"
#include "../../hardware/hardware_manager.h"
#include "../../system/menu_setting_table.h"
#include "../../system/nvs_settings_store.h"
#include "../event_bridge_lvgl.h"
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
    }

    LOG_DEBUG_PRINTLN(enabled ? def.log_on : def.log_off);
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
