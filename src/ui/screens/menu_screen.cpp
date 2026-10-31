#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../../logging/grind_logging.h"
#include "../../system/statistics_manager.h"
#include "../../hardware/hardware_manager.h"
#include "grinding_screen.h"
#include "../event_bridge_lvgl.h"
#include "../../config/logging.h"
#include "../components/blocking_overlay.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

static void back_event_handler(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target_obj(e);
    lv_obj_t * menu = (lv_obj_t *)lv_event_get_user_data(e);

    if(lv_menu_back_button_is_root(menu, obj)) {
        // Call the EventBridge handle_event directly with MENU_BACK event
        EventBridgeLVGL::handle_event(EventBridgeLVGL::EventType::MENU_BACK, e);
    }
}

void MenuScreen::create(BluetoothManager* bluetooth, GrindController* grind_ctrl, GrindingScreen* grind_screen, class HardwareManager* hw_mgr, DiagnosticsController* diag_ctrl) {
    bluetooth_manager = bluetooth;
    grind_controller = grind_ctrl;
    grinding_screen = grind_screen;
    hardware_manager = hw_mgr;
    diagnostics_controller = diag_ctrl;

    screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
    lv_obj_align(screen, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_set_hidden(screen, true);

    // Create menu UI immediately at boot for instant access
    create_menu_ui();
}

void MenuScreen::create_menu_ui() {
    if (menu) {
        return; // Already created
    }

    LOG_BLE("[%lums MENU] Creating menu UI\n", millis());

    // Create menu instead of tabview
    menu = lv_menu_create(screen);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_align(menu, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(menu, 0, 0);
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_ENABLED);
    lv_obj_add_event_cb(menu, back_event_handler, LV_EVENT_CLICKED, menu);

    // Get header and set up flex layout
    lv_obj_t* header = lv_menu_get_main_header(menu);
    lv_obj_set_layout(header, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int32_t title_base_height = lv_font_get_line_height(&lv_font_montserrat_36);
    int32_t title_target_height = (title_base_height * 3) / 2; // ~50% taller
    int32_t title_padding = (title_target_height - title_base_height) / 2;
    lv_obj_set_style_min_height(header, title_target_height, 0);
    lv_obj_set_style_pad_top(header, title_padding, 0);
    lv_obj_set_style_pad_bottom(header, title_padding, 0);

    // Get the header label
    lv_obj_t* header_label = lv_obj_get_child(header, -1);
    if (header_label) {
        lv_obj_set_style_text_align(header_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(header_label, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_color(header_label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_min_height(header_label, title_target_height, 0);
        lv_obj_set_style_pad_top(header_label, title_padding, 0);
        lv_obj_set_style_pad_bottom(header_label, title_padding, 0);
    }
    // Get and style the chevron first
    lv_obj_t* back_chevron = lv_menu_get_main_header_back_button(menu);
    lv_obj_set_style_text_font(back_chevron, &lv_font_montserrat_32, 0);
    lv_obj_set_ext_click_area(back_chevron, 200);
    lv_obj_set_style_text_color(back_chevron, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_min_height(back_chevron, title_target_height, 0);
    lv_obj_set_style_pad_top(back_chevron, title_padding, 0);
    lv_obj_set_style_pad_bottom(back_chevron, title_padding, 0);

    // Force layout update to get proper chevron dimensions
    lv_obj_update_layout(header);

    // Create spacer using chevron's actual size
    // Spacer is used to center name of the page name in the header
    lv_obj_t* spacer = lv_obj_create(header);
    lv_obj_set_size(spacer, lv_obj_get_width(back_chevron), lv_obj_get_height(back_chevron));
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);

    // Create main page last
    lv_obj_t* main_page = lv_menu_page_create(menu, "Menu");
    lv_obj_set_layout(main_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_gap(main_page, 0, 0);
    lv_obj_set_scroll_dir(main_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(main_page, LV_SCROLLBAR_MODE_AUTO);

    // Create sub-pages with titles
    info_page = lv_menu_page_create(menu, "Info");
    create_info_page(info_page);

    bluetooth_page = lv_menu_page_create(menu, "Bluetooth");
    create_bluetooth_page(bluetooth_page);

    wifi_page = lv_menu_page_create(menu, "WiFi");
    create_wifi_page(wifi_page);

    display_page = lv_menu_page_create(menu, "Display");
    create_display_page(display_page);

    grind_mode_page = lv_menu_page_create(menu, "Grind Settings");
    create_grind_mode_page(grind_mode_page);
    
    board_page = lv_menu_page_create(menu, "Hardware board");
    create_board_page(board_page);

    scale_page = lv_menu_page_create(menu, "Scale");
    create_scale_page(scale_page);

    data_page = lv_menu_page_create(menu, "Logs & Data");
    create_data_page(data_page);

    stats_page = lv_menu_page_create(menu, "Lifetime Stats");
    create_stats_page(stats_page);

    diagnostics_page = lv_menu_page_create(menu, "Diagnostics");
    create_diagnostics_page(diagnostics_page);

    // Main-page rows, grouped by section. `item` keeps the row handle for entries other code needs, `page` is
    // the sub-page a tap opens, `action` an event raised on tap (kNoAction = none).
    using ET = EventBridgeLVGL::EventType;
    constexpr ET kNoAction = static_cast<ET>(-1);
    struct MainItem {
        const char* section;
        const char* label;
        lv_obj_t* MenuScreen::* item;
        lv_obj_t* MenuScreen::* page;
        ET action;
    };
    const MainItem main_items[] = {
        {"Tools",    "Scale",          &MenuScreen::scale_item,        &MenuScreen::scale_page,       kNoAction},
        {"Tools",    "Calibrate",      &MenuScreen::cal_button,        nullptr,                       ET::MENU_CALIBRATE},
        {"Tools",    "Tune Pulses",    &MenuScreen::autotune_button,   nullptr,                       ET::MENU_AUTOTUNE},
        {"Tools",    "Motor Test",     &MenuScreen::motor_test_button, nullptr,                       ET::MENU_MOTOR_TEST},
        {"Settings", "Bluetooth",      nullptr,                        &MenuScreen::bluetooth_page,   kNoAction},
        {"Settings", "WiFi",           nullptr,                        &MenuScreen::wifi_page,        kNoAction},
        {"Settings", "Display",        nullptr,                        &MenuScreen::display_page,     kNoAction},
        {"Settings", "Grind Settings", nullptr,                        &MenuScreen::grind_mode_page,  kNoAction},
        {"Settings", "Hardware board", nullptr,                        &MenuScreen::board_page,       kNoAction},
        {"Info",     "Diagnostics",    nullptr,                        &MenuScreen::diagnostics_page, kNoAction},
        {"Info",     "System Info",    nullptr,                        &MenuScreen::info_page,        kNoAction},
        {"Info",     "Logs & Data",    nullptr,                        &MenuScreen::data_page,        kNoAction},
        {"Info",     "Lifetime Stats", nullptr,                        &MenuScreen::stats_page,       kNoAction},
    };
    const char* current_section = nullptr;
    for (const MainItem& entry : main_items) {
        if (entry.section != current_section) {
            current_section = entry.section;
            create_separator(main_page, current_section);
        }
        lv_obj_t* row = create_menu_item(main_page, entry.label);
        if (entry.item) this->*(entry.item) = row;
        if (entry.page) lv_menu_set_load_page_event(menu, row, this->*(entry.page));
        lv_obj_set_clickable(row, true);
        if (entry.action != kNoAction) bind_event(row, LV_EVENT_CLICKED, entry.action);
    }


    // Set main page as active (menu will be the landing page)
    lv_menu_set_page(menu, main_page);

    // Refresh lifetime/log statistics whenever the Data or Stats page is displayed
    auto changing_page_callback = [](lv_event_t * e) {
        MenuScreen * self = static_cast<MenuScreen*>(lv_event_get_user_data(e));
        lv_obj_t * menu = static_cast<lv_obj_t *>(lv_event_get_target(e));
        lv_obj_t * cur = lv_menu_get_cur_main_page(menu);
        if (cur == self->data_page || cur == self->stats_page) {
            self->refresh_statistics();
        }

        bool on_scale = (cur == self->scale_page);
        if (on_scale) {
            if (!self->scale_active) {
                self->scale_active = true;
                self->reset_scale_display();
                EventBridgeLVGL::handle_event(EventBridgeLVGL::EventType::MENU_SCALE_OPEN, e);
            }
        } else if (self->scale_active) {
            self->scale_active = false;
        }

        self->sync_wifi_provisioning_window();
    };

    lv_obj_add_event_cb(menu, changing_page_callback, LV_EVENT_VALUE_CHANGED, this);

    LOG_BLE("[%lums MENU] Menu UI created successfully\n", millis());
}

void MenuScreen::show() {
    LOG_BLE("[%lums MENU] Showing menu screen\n", millis());

    lv_obj_set_hidden(screen, false);
    visible = true;
    update_ble_status();
    update_brightness_sliders();
    update_bluetooth_startup_toggle();
    update_logging_toggle();
    update_grind_mode_toggles();
    update_board_page();
    // The WiFi page can still be the current page when the menu is shown again
    sync_wifi_provisioning_window();

    LOG_BLE("[%lums MENU] Menu screen shown successfully\n", millis());
}

void MenuScreen::hide() {
    if (!visible) {
        return; // Already hidden, nothing to do
    }

    LOG_BLE("[%lums MENU] Hiding menu screen\n", millis());

    // Keep the menu in memory for instant access next time
    // Just hide the screen instead of deleting the menu
    lv_obj_set_hidden(screen, true);
    visible = false;
    sync_wifi_provisioning_window();  // closes the BLE provisioning window
    LOG_BLE("[%lums MENU] Menu screen hidden successfully\n", millis());
}

bool MenuScreen::is_page_current(lv_obj_t* page) const {
    return visible && menu && page && lv_menu_get_cur_main_page(menu) == page;
}

bool MenuScreen::is_info_page_current() const { return is_page_current(info_page); }
bool MenuScreen::is_diagnostics_page_current() const { return is_page_current(diagnostics_page); }
bool MenuScreen::is_ble_page_current() const { return is_page_current(bluetooth_page); }
bool MenuScreen::is_wifi_page_current() const { return is_page_current(wifi_page); }

LV_DEPRECATIONS_IGNORE_END
