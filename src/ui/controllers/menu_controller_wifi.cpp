// MenuUIController: WiFi page handlers. Kept apart from menu_controller.cpp for size.
#include "menu_controller.h"

#include <Arduino.h>
#include <string>
#include "../../config/constants.h"
#include "../../config/logging.h"
#include "../../network/pull_ui_text.h"
#include "../../network/update_approval_gate.h"
#include "../../network/update_pull.h"
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

// "Beta": include prereleases when checking. Changing the channel invalidates the previous check result.
void MenuUIController::handle_wifi_beta_toggle() {
    if (!ui_manager_) return;
    auto* toggle = ui_manager_->menu_screen.get_wifi_beta_toggle();
    if (!toggle) return;

    const bool beta = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    auto revert = [toggle, beta]() {
        if (beta) lv_obj_clear_state(toggle, LV_STATE_CHECKED); else lv_obj_add_state(toggle, LV_STATE_CHECKED);
    };
    if (!pull_can_start_check(update_pull.state())) {  // a job is running: it keeps the channel it started with
        revert();
        return;
    }
    if (!wifi_manager.set_beta_channel(beta)) {
        revert();
        LOG_BLE("[UI] WiFi beta channel: could not store the setting\n");
        return;
    }
    update_pull.reset();
    ui_manager_->menu_screen.set_wifi_pull_notice(PullFailure::NONE);
    ui_manager_->menu_screen.update_wifi_page();
    LOG_BLE("[UI] WiFi update channel %s\n", beta ? "beta" : "stable");
}

void MenuUIController::handle_wifi_check_update() {
    if (!ui_manager_) return;
    auto& screen = ui_manager_->menu_screen;
    auto* button = screen.get_wifi_check_button();
    if (!button || lv_obj_has_state(button, LV_STATE_DISABLED)) return;

    if (!update_pull.request_check(wifi_manager.beta_channel())) {
        screen.set_wifi_pull_notice(update_pull.refusal());
    } else {
        screen.set_wifi_pull_notice(PullFailure::NONE);
    }
    screen.update_wifi_page();
}

// Install tap: re-check that the offer is still fresh, then ask. The dialog is the only way to start the install.
void MenuUIController::handle_wifi_install_update() {
    if (!ui_manager_ || !ui_manager_->state_machine || !ui_manager_->state_machine->is_state(UIState::MENU)) return;  // double tap
    auto& screen = ui_manager_->menu_screen;
    auto* button = screen.get_wifi_install_button();
    if (!button || !lv_obj_is_visible(button)) return;

    PullOffer offer;
    const bool fresh = update_pull.state() == PullState::AVAILABLE && update_pull.offer(&offer) &&
                       !pull_result_is_stale(update_pull.result_at_ms(), millis(), wifi_manager.is_connected());
    if (!fresh) {
        screen.update_wifi_page();  // hides the button and says why
        return;
    }
    const PullFailure refused = update_pull.install_refusal();
    if (refused != PullFailure::NONE) {
        screen.set_wifi_pull_notice(refused);
        return;
    }

    char message[PULL_UI_CONFIRM_BUF];
    pull_confirm_message(offer.version, BUILD_FIRMWARE_VERSION, offer.size, message, sizeof(message));
    const std::string version = offer.version;
    ui_manager_->show_confirmation(
        "UPDATE",
        message,
        "INSTALL",
        lv_color_hex(THEME_COLOR_ACCENT),
        [this, version]() { install_update_confirmed(version.c_str()); },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
    // A finger that was already on the screen must not hit INSTALL (same guard as the Accept dialog)
    if (ui_manager_->confirm_controller_) ui_manager_->confirm_controller_->guard_taps(UPDATE_UI_TAP_GUARD_MS);
}

// INSTALL pressed. Runs in the confirm dialog callback on the UI task: Bluetooth is switched off here (disable() is
// only safe from the UI / Bluetooth task), then the worker starts; WifiUpdateUIController shows the progress screen.
void MenuUIController::install_update_confirmed(const char* version) {
    if (!ui_manager_) return;
    auto& screen = ui_manager_->menu_screen;

    // Only the release the user saw is installed; a new check while the dialog was up replaces the offer
    PullOffer offer;
    const bool same = update_pull.state() == PullState::AVAILABLE && update_pull.offer(&offer) &&
                      strcmp(offer.version, version) == 0 &&
                      !pull_result_is_stale(update_pull.result_at_ms(), millis(), wifi_manager.is_connected());
    if (!same) {
        screen.set_wifi_pull_notice(PullFailure::NOT_AVAILABLE);
        return;
    }
    // Ask before Bluetooth goes off, so a refused install leaves it alone
    const PullFailure refused = update_pull.install_refusal();
    if (refused != PullFailure::NONE) {
        screen.set_wifi_pull_notice(refused);
        return;
    }

    auto* ble = ui_manager_->bluetooth_manager;
    const bool ble_was_on = ble && ble->is_enabled();
    if (ble_was_on) {
        ble->disable();
        screen.update_ble_status();
        LOG_BLE("[UI] Bluetooth turned off for the firmware download\n");
    }
    if (!update_pull.request_install()) {
        screen.set_wifi_pull_notice(update_pull.refusal());
        if (ble_was_on) {
            // Nothing started, so give Bluetooth back (same path as the Bluetooth toggle)
            auto operation = [ble]() { ble->enable(); };
            auto completion = [&screen]() { screen.update_ble_status(); };
            BlockingOperationOverlay::getInstance().show_and_execute(BlockingOperation::BLE_ENABLING, operation, completion);
            LOG_BLE("[UI] Install refused, Bluetooth turned on again\n");
        }
        return;
    }
    screen.set_wifi_pull_notice(PullFailure::NONE);
    LOG_BLE("[UI] firmware download started: v%s\n", version);
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
