// MenuUIController: WiFi page handlers. Kept apart from menu_controller.cpp for size.
#include "menu_controller.h"

#include <Arduino.h>
#include "../../config/constants.h"
#include "../../config/logging.h"
#include "../../network/update_approval_gate.h"
#include "../../network/wifi_manager.h"
#include "../event_bridge_lvgl.h"
#include "../ui_manager.h"
#include "../screens/menu_screen.h"

void MenuUIController::handle_wifi_forget() {
    if (!ui_manager_) return;

    ui_manager_->show_confirmation(
        "FORGET WIFI",
        "Removes the saved network and the update token. "
        "Updates over WiFi stop until you set it up again.",
        "FORGET",
        lv_color_hex(THEME_COLOR_ERROR),
        []() {
            if (wifi_manager.clear_credentials()) {
                update_approval.cancel();  // a request waiting for the user is moot without credentials
                LOG_BLE("[UI] WiFi forgotten by user\n");
            }
        },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
}

// "Receive": arms / disarms the push endpoint (the server follows within its 2 s gate check)
void MenuUIController::handle_wifi_push_toggle() {
    if (!ui_manager_) return;
    auto* toggle = ui_manager_->menu_screen.get_wifi_push_toggle();
    if (!toggle) return;

    const bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    if (!wifi_manager.set_push_enabled(enabled)) {
        // Not stored: show the real state again
        if (enabled) lv_obj_clear_state(toggle, LV_STATE_CHECKED); else lv_obj_add_state(toggle, LV_STATE_CHECKED);
        LOG_BLE("[UI] WiFi receive: could not store the setting\n");
        return;
    }
    if (!enabled) update_approval.cancel();  // a request waiting for the user is withdrawn
    LOG_BLE("[UI] WiFi receive %s\n", enabled ? "on" : "off");
}

// "Confirm": ON applies at once; OFF removes the physical control, so it needs a warning dialog first
void MenuUIController::handle_wifi_confirm_toggle() {
    if (!ui_manager_) return;
    auto* toggle = ui_manager_->menu_screen.get_wifi_confirm_toggle();
    if (!toggle) return;

    if (lv_obj_has_state(toggle, LV_STATE_CHECKED)) {
        wifi_manager.set_accept_required(true);
        LOG_BLE("[UI] WiFi confirm on\n");
        return;
    }

    lv_obj_add_state(toggle, LV_STATE_CHECKED);  // stays on until the user confirms; the page re-syncs on return
    ui_manager_->show_confirmation(
        "TURN OFF CONFIRM",
        "Anyone on your network who has the update token will be able to install firmware "
        "without touching the grinder.",
        "TURN OFF",
        lv_color_hex(THEME_COLOR_WARNING),
        []() {
            wifi_manager.set_accept_required(false);
            LOG_BLE("[UI] WiFi confirm off by user\n");
        },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
}

void MenuUIController::update_wifi_page() {
    if (!ui_manager_ || !ui_manager_->menu_screen.is_wifi_page_current()) return;

    // Runtime values only; the stored SSID / credentials flag are re-read on page entry and after Forget
    const uint32_t now = millis();
    if (now - last_wifi_refresh_ms_ < WIFI_PAGE_REFRESH_INTERVAL_MS) return;
    last_wifi_refresh_ms_ = now;
    // Credentials changed since the last load (e.g. provisioned or cleared over BLE while the page is open)
    if (ui_manager_->menu_screen.wifi_credentials_changed()) {
        ui_manager_->menu_screen.reload_wifi_page();  // also refreshes the runtime values
    } else {
        ui_manager_->menu_screen.update_wifi_page();
    }
}
