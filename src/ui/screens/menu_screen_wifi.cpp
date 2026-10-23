// MenuScreen: WiFi page (status, Forget, BLE provisioning window). Kept apart from menu_screen.cpp for size.
#include "menu_screen.h"
#include <Arduino.h>
#include <string.h>
#include "../../config/constants.h"
#include "../../config/logging.h"
#include "../../network/update_server.h"
#include "../../network/update_ui_logic.h"
#include "../../network/wifi_manager.h"
#include "../event_bridge_lvgl.h"

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
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    return block;
}

// lv_label_set_text always invalidates; this page refreshes once a second, so skip unchanged text
void set_text_if_changed(lv_obj_t* label, const char* text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void set_visible(lv_obj_t* obj, bool show) {
    if (!obj) return;
    if (show) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
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
                             "wifi-upload). Each update must be accepted on this screen.");
    create_toggle_row(parent, "Receive", &wifi_push_toggle);
    create_description_label(parent,
                             "Ask before installing. Turning this off lets anyone with the update token "
                             "install firmware without touching the grinder.");
    create_toggle_row(parent, "Confirm", &wifi_confirm_toggle);
    wifi_listen_label = create_description_label(parent, "");
    lv_obj_set_style_text_color(wifi_listen_label, lv_color_hex(THEME_COLOR_SUCCESS), 0);

    using ET = EventBridgeLVGL::EventType;
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
            lv_obj_clear_state(toggle, LV_STATE_CHECKED);
        }
        if (wifi_has_credentials) {
            lv_obj_clear_state(toggle, LV_STATE_DISABLED);
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
    update_wifi_page();
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
    // The label sits in its own padded container (create_description_label): hide that, not just the text
    set_visible(wifi_ble_warning_label ? lv_obj_get_parent(wifi_ble_warning_label) : nullptr,
                bluetooth_manager && !bluetooth_manager->is_enabled());
}
