// MenuScreen: table-driven setting toggles (see system/menu_setting_table.h).
#include "menu_screen.h"
#include "../../config/constants.h"
#include "../../system/nvs_settings_store.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

// Indexed by ToggleSetting
static const EventBridgeLVGL::EventType kSettingToggleEvents[TOGGLE_SETTING_COUNT] = {
    EventBridgeLVGL::EventType::BLE_STARTUP_TOGGLE,
    EventBridgeLVGL::EventType::LOGGING_TOGGLE,
    EventBridgeLVGL::EventType::GRIND_MODE_SWIPE_TOGGLE,
    EventBridgeLVGL::EventType::AUTO_START_TOGGLE,
    EventBridgeLVGL::EventType::AUTO_RETURN_TOGGLE,
};

EventBridgeLVGL::EventType MenuScreen::setting_toggle_event(ToggleSetting id) {
    return kSettingToggleEvents[static_cast<size_t>(id)];
}

void MenuScreen::create_setting_toggle_row(lv_obj_t* parent, ToggleSetting id) {
    lv_obj_t** toggle = &setting_toggles_[static_cast<size_t>(id)];
    create_toggle_row(parent, toggle_setting_def(id).label, toggle);
    bind_event(*toggle, LV_EVENT_VALUE_CHANGED, setting_toggle_event(id));
}

void MenuScreen::refresh_setting_toggles() {
    for (size_t i = 0; i < TOGGLE_SETTING_COUNT; i++) {
        lv_obj_t* toggle = setting_toggles_[i];
        if (!toggle) continue;

        if ((ui_settings.*toggle_setting_def(static_cast<ToggleSetting>(i)).get)()) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
    }
}

LV_DEPRECATIONS_IGNORE_END
