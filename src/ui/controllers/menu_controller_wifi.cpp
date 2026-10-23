// MenuUIController: WiFi page handlers. Kept apart from menu_controller.cpp for size.
#include "menu_controller.h"

#include <Arduino.h>
#include "../../config/constants.h"
#include "../../config/logging.h"
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
            if (wifi_manager.clear_credentials()) LOG_BLE("[UI] WiFi forgotten by user\n");
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
