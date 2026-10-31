// MenuUIController: table-driven setting toggles (see system/menu_setting_table.h).
#include "menu_controller.h"

#include "../../config/constants.h"
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
