// MenuScreen: WiFi page (status, Forget, BLE provisioning window). Kept apart from menu_screen.cpp for size.
#include "menu_screen.h"
#include <Arduino.h>
#include <string.h>
#include "../../config/constants.h"
#include "../../config/logging.h"
#include "../../network/update_pull.h"
#include "../../network/update_server.h"
#include "../../network/update_ui_logic.h"
#include "../../network/wifi_manager.h"
#include "../event_bridge_lvgl.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

namespace {

// Transparent full-width column that groups the widgets shown/hidden together
lv_obj_t* create_block(lv_obj_t* parent) {
    lv_obj_t* block = lv_obj_create(parent);
    lv_obj_set_size(block, 280, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_gap(block, 0, 0);
    lv_obj_set_layout(block, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(block, false);
    return block;
}

// lv_label_set_text always invalidates; this page refreshes once a second, so skip unchanged text
void set_text_if_changed(lv_obj_t* label, const char* text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// Wraps the button text inside the button (a long "Install v1.5.0-rc.3" breaks into two centred lines)
void wrap_button_text(lv_obj_t* button) {
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (!label) return;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, 220);  // 260 px button minus 2 x 20 px padding
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

void set_enabled(lv_obj_t* obj, bool enabled) {
    if (!obj) return;
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

void set_visible(lv_obj_t* obj, bool show) {
    if (!obj) return;
    if (show) {
        lv_obj_set_hidden(obj, false);
    } else {
        lv_obj_set_hidden(obj, true);
    }
}

}  // namespace

void MenuScreen::create_wifi_page(lv_obj_t* parent) {
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

    create_description_label(parent, "Connect to your home WiFi to update the firmware without Bluetooth.");

    create_separator(parent, "Status");
    create_data_label(parent, "Status:", &wifi_status_label);
    create_data_label(parent, "Network:", &wifi_network_label, true);
    create_data_label(parent, "IP:", &wifi_ip_label);
    create_data_label(parent, "Signal:", &wifi_signal_label, true);
    create_data_label(parent, "Name:", &wifi_name_label, true);

    // Block A: nothing stored yet
    wifi_setup_block = create_block(parent);
    create_description_label(wifi_setup_block,
                             "Not set up. Send your WiFi details over Bluetooth with the web flasher "
                             "(WiFi setup tab) or python3 tools/grinder.py wifi-set.");
    wifi_ble_warning_label = create_description_label(wifi_setup_block, "Turn Bluetooth on first.");
    lv_obj_set_style_text_color(wifi_ble_warning_label, lv_color_hex(THEME_COLOR_WARNING), 0);

    // Block B: credentials stored
    wifi_manage_block = create_block(parent);
    create_description_label(wifi_manage_block,
                             "To change the network, send new details over Bluetooth while this page is open.");
    wifi_forget_button = create_button(wifi_manage_block, "Forget WiFi", lv_color_hex(THEME_COLOR_ERROR));
    lv_obj_set_style_margin_bottom(wifi_forget_button, 10, 0);

    if (wifi_forget_button) {
        lv_obj_add_event_cb(wifi_forget_button, EventBridgeLVGL::dispatch_event, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(EventBridgeLVGL::EventType::WIFI_FORGET)));
    }

    // Updates: arm the push endpoint and decide whether each update needs a tap on the grinder
    create_separator(parent, "Updates");
    create_description_label(parent,
                             "Lets your computer send firmware over your network (python3 tools/grinder.py "
                             "wifi-upload).");
    create_toggle_row(parent, "Receive", &wifi_push_toggle);
    // Confirm only matters while Receive is on: its text and toggle are hidden together (update_wifi_page)
    wifi_confirm_block = create_block(parent);
    create_description_label(wifi_confirm_block,
                             "Ask before installing. Turning this off lets anyone with the update token "
                             "install firmware without touching the grinder.");
    create_toggle_row(wifi_confirm_block, "Confirm", &wifi_confirm_toggle);
    wifi_listen_label = create_description_label(parent, "");
    lv_obj_set_style_text_color(wifi_listen_label, lv_color_hex(THEME_COLOR_SUCCESS), 0);

    // Firmware: check a release channel and install over WiFi (R7)
    create_separator(parent, "Firmware");
    lv_obj_t* installed_label = nullptr;
    create_data_label(parent, "Installed:", &installed_label, true);
    char installed[PULL_UI_VERSION_BUF + 16];
    lv_label_set_text(installed_label, pull_installed_text(BUILD_FIRMWARE_VERSION, BUILD_NUMBER, installed, sizeof(installed)));
    create_data_label(parent, "Latest:", &wifi_fw_latest_label, true);
    create_description_label(parent, "Include pre-releases (RC, beta) when checking.");
    create_toggle_row(parent, "Beta", &wifi_beta_toggle);
    wifi_check_button = create_button(parent, "Check for update", lv_color_hex(THEME_COLOR_ACCENT));
    wifi_install_button = create_button(parent, "Install", lv_color_hex(THEME_COLOR_SUCCESS));
    for (lv_obj_t* button : {wifi_check_button, wifi_install_button}) {
        lv_obj_set_style_margin_bottom(button, 10, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    }
    wrap_button_text(wifi_install_button);  // only the version label can get long
    set_visible(wifi_install_button, false);
    wifi_fw_notice_label = create_description_label(parent, "");
    lv_obj_set_style_text_color(wifi_fw_notice_label, lv_color_hex(THEME_COLOR_WARNING), 0);

    using ET = EventBridgeLVGL::EventType;
    if (wifi_beta_toggle) {
        lv_obj_add_event_cb(wifi_beta_toggle, EventBridgeLVGL::dispatch_event, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(ET::WIFI_BETA_TOGGLE)));
    }
    if (wifi_check_button) {
        lv_obj_add_event_cb(wifi_check_button, EventBridgeLVGL::dispatch_event, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(ET::WIFI_CHECK_UPDATE)));
    }
    if (wifi_install_button) {
        lv_obj_add_event_cb(wifi_install_button, EventBridgeLVGL::dispatch_event, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(ET::WIFI_INSTALL_UPDATE)));
    }
    if (wifi_push_toggle) {
        lv_obj_add_event_cb(wifi_push_toggle, EventBridgeLVGL::dispatch_event, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(ET::WIFI_PUSH_TOGGLE)));
    }
    if (wifi_confirm_toggle) {
        lv_obj_add_event_cb(wifi_confirm_toggle, EventBridgeLVGL::dispatch_event, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(ET::WIFI_CONFIRM_TOGGLE)));
    }
    // Content is filled in by reload_wifi_page() when the page is entered
}

// Toggle states come from NVS / the RAM copy of the Confirm flag. Without credentials both are greyed out and
// off/default: arming needs a token, and an unattended Confirm-off must not survive into a later provisioning.
void MenuScreen::apply_wifi_update_toggles() {
    auto apply = [this](lv_obj_t* toggle, bool on) {
        if (!toggle) return;
        if (on) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        if (wifi_has_credentials) {
            lv_obj_remove_state(toggle, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(toggle, LV_STATE_DISABLED);
        }
    };
    apply(wifi_push_toggle, wifi_has_credentials && wifi_manager.push_enabled());
    apply(wifi_confirm_toggle, wifi_manager.accept_required());
}

bool MenuScreen::is_wifi_page_current() const {
    return visible && menu && wifi_page && lv_menu_get_cur_main_page(menu) == wifi_page;
}

bool MenuScreen::wifi_credentials_changed() const {
    return wifi_loaded_revision != wifi_manager.credentials_revision();
}

void MenuScreen::sync_wifi_provisioning_window() {
    const bool want_open = is_wifi_page_current();
    if (want_open == wifi_page_active) return;

    wifi_page_active = want_open;
    WiFiManager::set_provisioning_window(want_open);
    LOG_BLE("[UI] WiFi page %s (provisioning window %s)\n", want_open ? "entered" : "left",
            want_open ? "open" : "closed");
    if (want_open) reload_wifi_page();
}

void MenuScreen::reload_wifi_page() {
    wifi_loaded_revision = wifi_manager.credentials_revision();
    wifi_has_credentials = wifi_manager.has_credentials();
    char ssid[WIFI_SSID_MAX_LEN + 1];
    wifi_manager.get_ssid(ssid, sizeof(ssid));
    wifi_shorten_ssid(ssid, wifi_ssid_display, sizeof(wifi_ssid_display));
    apply_wifi_update_toggles();
    // The beta choice is stored; the check result lives in update_pull, so it is simply shown again. A refusal
    // message survives (it may come from the Install dialog that was just closed) and expires by itself.
    if (wifi_beta_toggle) {
        if (wifi_manager.beta_channel()) {
            lv_obj_add_state(wifi_beta_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(wifi_beta_toggle, LV_STATE_CHECKED);
        }
    }
    update_wifi_page();
}

void MenuScreen::set_wifi_pull_notice(PullFailure why) {
    wifi_pull_notice = why;
    wifi_pull_notice_ms = millis();
    update_wifi_firmware();
}

void MenuScreen::update_wifi_page() {
    if (!wifi_status_label) return;

    // Right after Forget the radio still reports its old state until the Bluetooth task tears it down
    const WiFiState state = wifi_has_credentials ? wifi_manager.state() : WiFiState::NO_CREDENTIALS;
    const bool connected = state == WiFiState::CONNECTED;

    char text[WIFI_STATUS_TEXT_BUF];
    set_text_if_changed(wifi_status_label, wifi_status_text(state, wifi_manager.retry_in_ms(), text, sizeof(text)));
    uint32_t color = THEME_COLOR_TEXT_SECONDARY;
    if (connected) {
        color = THEME_COLOR_SUCCESS;
    } else if (state != WiFiState::NO_CREDENTIALS) {
        color = THEME_COLOR_WARNING;
    }
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(color), 0);

    set_text_if_changed(wifi_network_label, wifi_has_credentials ? wifi_ssid_display : "--");

    if (connected) {
        char ip[16];
        wifi_manager.get_ip(ip, sizeof(ip));
        set_text_if_changed(wifi_ip_label, ip);
        set_text_if_changed(wifi_signal_label, wifi_signal_text(wifi_manager.rssi(), text, sizeof(text)));
    } else {
        set_text_if_changed(wifi_ip_label, "--");
        set_text_if_changed(wifi_signal_label, "--");
    }

    char name[WIFI_HOSTNAME_MAX_LEN + 8];
    snprintf(name, sizeof(name), "%s.local", wifi_manager.hostname());
    set_text_if_changed(wifi_name_label, name);

    const bool listening = wifi_has_credentials && update_server.running();
    if (listening) {
        char listen_text[96];
        set_text_if_changed(wifi_listen_label, update_listening_text(wifi_manager.hostname(), listen_text, sizeof(listen_text)));
    }
    set_visible(wifi_listen_label ? lv_obj_get_parent(wifi_listen_label) : nullptr, listening);

    set_visible(wifi_setup_block, !wifi_has_credentials);
    set_visible(wifi_manage_block, wifi_has_credentials);
    // Receive off hides Confirm (not just greys it); the stored Confirm value is untouched
    set_visible(wifi_confirm_block, wifi_push_toggle && lv_obj_has_state(wifi_push_toggle, LV_STATE_CHECKED));
    // The label sits in its own padded container (create_description_label): hide that, not just the text
    set_visible(wifi_ble_warning_label ? lv_obj_get_parent(wifi_ble_warning_label) : nullptr,
                bluetooth_manager && !bluetooth_manager->is_enabled());

    update_wifi_firmware();
}

// Firmware section: everything shown comes from update_pull (state, offer, failure) through the pure pull_page_view()
void MenuScreen::update_wifi_firmware() {
    if (!wifi_fw_latest_label) return;

    const uint32_t now = millis();
    if (wifi_pull_notice != PullFailure::NONE && now - wifi_pull_notice_ms > UPDATE_PULL_NOTICE_MS) {
        wifi_pull_notice = PullFailure::NONE;
    }

    PullOffer offer;
    const bool have_offer = update_pull.offer(&offer);
    PullPageInput in;
    in.state = update_pull.state();
    in.failure = update_pull.failure();
    in.notice = wifi_pull_notice;
    in.no_release = update_pull.no_release();
    in.wifi_connected = wifi_manager.is_connected();
    in.ble_enabled = bluetooth_manager && bluetooth_manager->is_enabled();
    in.offer_version = have_offer ? offer.version : nullptr;
    in.result_ms = update_pull.result_at_ms();
    in.now_ms = now;
    PullPageView view;
    pull_page_view(in, &view);

    uint32_t tone = THEME_COLOR_TEXT_SECONDARY;
    if (view.latest_tone == PullTone::GOOD) tone = THEME_COLOR_SUCCESS;
    if (view.latest_tone == PullTone::WARN) tone = THEME_COLOR_WARNING;
    set_text_if_changed(wifi_fw_latest_label, view.latest);
    lv_obj_set_style_text_color(wifi_fw_latest_label, lv_color_hex(tone), 0);

    set_text_if_changed(lv_obj_get_child(wifi_check_button, 0), view.check_label);
    set_enabled(wifi_check_button, view.check_enabled);
    set_enabled(wifi_beta_toggle, view.beta_enabled);

    set_visible(wifi_install_button, view.show_install);
    if (view.show_install) set_text_if_changed(lv_obj_get_child(wifi_install_button, 0), view.install_label);

    set_text_if_changed(wifi_fw_notice_label, view.notice);
    // The label sits in its own padded container (create_description_label): hide that, not just the text
    set_visible(lv_obj_get_parent(wifi_fw_notice_label), view.notice[0] != '\0');
}

LV_DEPRECATIONS_IGNORE_END
