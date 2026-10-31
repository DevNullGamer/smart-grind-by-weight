// MenuScreen: Bluetooth page.
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../../system/nvs_settings_store.h"
#include "../event_bridge_lvgl.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_bluetooth_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_toggle_row(parent, "Enabled", &ble_toggle);
    create_toggle_row(parent, "Startup", &ble_startup_toggle);

    // BLE Status label
    ble_status_label = lv_label_create(parent);
    lv_label_set_text(ble_status_label, "Bluetooth: Disabled");
    lv_obj_set_style_text_font(ble_status_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ble_status_label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_scrollable(ble_status_label, false);

    // BLE Timer label
    ble_timer_label = lv_label_create(parent);
    lv_label_set_text(ble_timer_label, "");
    lv_obj_set_style_text_font(ble_timer_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ble_timer_label, lv_color_hex(THEME_COLOR_WARNING), 0);
    lv_obj_set_scrollable(ble_timer_label, false);

    // Register events for the toggles (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(ble_toggle, LV_EVENT_VALUE_CHANGED, ET::BLE_TOGGLE);
    bind_event(ble_startup_toggle, LV_EVENT_VALUE_CHANGED, ET::BLE_STARTUP_TOGGLE);
}

void MenuScreen::update_ble_status() {
    if (!visible || !bluetooth_manager) return;
    
    // Update toggle state
    if (bluetooth_manager->is_enabled()) {
        lv_obj_add_state(ble_toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(ble_toggle, LV_STATE_CHECKED);
    }
    
    // Update status text
    if (bluetooth_manager->is_enabled()) {
        if (bluetooth_manager->is_connected()) {
            set_label_text_if_changed(ble_status_label, "Connected");
        } else {
            set_label_text_if_changed(ble_status_label, "Advertising");
        }
        lv_obj_set_hidden(ble_status_label, false);
        
        // Show remaining time
        unsigned long remaining_ms = bluetooth_manager->get_bluetooth_timeout_remaining_ms();
        unsigned long remaining_min = remaining_ms / (60 * 1000);
        char timer_text[64];
        snprintf(timer_text, sizeof(timer_text), "Auto-disable in: %lu min", remaining_min);
        set_label_text_if_changed(ble_timer_label, timer_text);
        lv_obj_set_hidden(ble_timer_label, false);
    } else {
        // When nothing to display hide the status labels
        lv_obj_set_hidden(ble_status_label, true);
        lv_obj_set_hidden(ble_timer_label, true);
    }
}

void MenuScreen::update_bluetooth_startup_toggle() {
    if (!ble_startup_toggle) return;

    // Stored startup flag (the facade defaults to true when unset)
    const bool startup_enabled = ui_settings.ble_startup();

    // Update toggle state
    if (startup_enabled) {
        lv_obj_add_state(ble_startup_toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(ble_startup_toggle, LV_STATE_CHECKED);
    }
}

LV_DEPRECATIONS_IGNORE_END
