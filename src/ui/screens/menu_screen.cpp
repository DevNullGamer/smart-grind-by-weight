#include "menu_screen.h"
#include <Arduino.h>
#include <algorithm>
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

    visible = false;
    scale_active = false;
    scale_page = nullptr;
    scale_weight_label = nullptr;
    scale_tare_button = nullptr;
    scale_item = nullptr;
    grinder_purge_mode_radio_group = nullptr;
    grinder_purge_amount_slider = nullptr;
    grinder_purge_amount_label = nullptr;
    grind_freshness_hours_slider = nullptr;
    grind_freshness_hours_label = nullptr;
    board_status_label = nullptr;
    board_override_radio_group = nullptr;
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

    // Create menu items grouped with separators
    create_separator(main_page, "Tools");
    scale_item = create_menu_item(main_page, "Scale");
    cal_button = create_menu_item(main_page, "Calibrate");
    autotune_button = create_menu_item(main_page, "Tune Pulses");
    motor_test_button = create_menu_item(main_page, "Motor Test");

    lv_menu_set_load_page_event(menu, scale_item, scale_page);

    lv_obj_set_clickable(scale_item, true);
    lv_obj_set_clickable(cal_button, true);
    lv_obj_set_clickable(autotune_button, true);
    lv_obj_set_clickable(motor_test_button, true);

    using ET = EventBridgeLVGL::EventType;
    bind_event(cal_button, LV_EVENT_CLICKED, ET::MENU_CALIBRATE);
    bind_event(autotune_button, LV_EVENT_CLICKED, ET::MENU_AUTOTUNE);
    bind_event(motor_test_button, LV_EVENT_CLICKED, ET::MENU_MOTOR_TEST);

    create_separator(main_page, "Settings");
    lv_obj_t* bluetooth_item = create_menu_item(main_page, "Bluetooth");
    lv_menu_set_load_page_event(menu, bluetooth_item, bluetooth_page);

    lv_obj_t* wifi_item = create_menu_item(main_page, "WiFi");
    lv_menu_set_load_page_event(menu, wifi_item, wifi_page);

    lv_obj_t* display_item = create_menu_item(main_page, "Display");
    lv_menu_set_load_page_event(menu, display_item, display_page);

    lv_obj_t* grind_mode_item = create_menu_item(main_page, "Grind Settings");
    lv_menu_set_load_page_event(menu, grind_mode_item, grind_mode_page);

    lv_obj_t* board_item = create_menu_item(main_page, "Hardware board");
    lv_menu_set_load_page_event(menu, board_item, board_page);

    create_separator(main_page, "Info");
    lv_obj_t* diagnostics_item = create_menu_item(main_page, "Diagnostics");
    lv_menu_set_load_page_event(menu, diagnostics_item, diagnostics_page);

    lv_obj_t* info_item = create_menu_item(main_page, "System Info");
    lv_menu_set_load_page_event(menu, info_item, info_page);

    lv_obj_t* data_item = create_menu_item(main_page, "Logs & Data");
    lv_menu_set_load_page_event(menu, data_item, data_page);

    lv_obj_t* stats_item = create_menu_item(main_page, "Lifetime Stats");
    lv_menu_set_load_page_event(menu, stats_item, stats_page);

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

// Callback for grind mode radio button selection
static void grind_mode_callback(int selected_index, void* user_data) {
    // Trigger the event system instead of handling directly
    EventBridgeLVGL::handle_event(EventBridgeLVGL::EventType::GRIND_MODE_RADIO_BUTTON, nullptr);
}

// Callback for grinder purge mode radio button selection
static void grinder_purge_mode_callback(int selected_index, void* user_data) {
    // Trigger the event system instead of handling directly
    EventBridgeLVGL::handle_event(EventBridgeLVGL::EventType::GRINDER_PURGE_MODE_RADIO_BUTTON, nullptr);
}

void MenuScreen::create_grind_mode_page(lv_obj_t* parent) {
    init_column_page(parent);

    // Mode Selection separator/label
    create_separator(parent, "Mode Selection");

    // Radio button group for grind mode selection at top
    const char* grind_modes[] = {"Weight", "Time"};
    grind_mode_radio_group = create_radio_button_group(
        parent,
        grind_modes,
        2,
        LV_FLEX_FLOW_ROW,
        0,  // Weight initially selected
        135, 100,  // Width, Height
        grind_mode_callback,
        this
    );

    // Descriptive label for swipe functionality
    lv_obj_t* swipe_desc_label = lv_label_create(parent);
    lv_label_set_text(swipe_desc_label, "Enable swiping vertically to switch between Weight/Time modes");
    lv_obj_set_style_text_font(swipe_desc_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(swipe_desc_label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_margin_bottom(swipe_desc_label, 10, 0);
    lv_label_set_long_mode(swipe_desc_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(swipe_desc_label, THEME_CONTENT_INNER_WIDTH_PX);

    // Swipe toggle using existing pattern
    create_toggle_row(parent, "Swipe", &grind_mode_swipe_toggle);

    // Automatic actions section
    create_separator(parent, "Automation");
    create_description_label(parent, "Start the selected profile as soon as the cup lands on the scale.");
    create_toggle_row(parent, "Start", &auto_start_toggle);
    create_description_label(parent, "Exit the completion screen once that cup weight drops away.");
    create_toggle_row(parent, "Return", &auto_return_toggle);

    // Grinder Purging section
    create_separator(parent, "Purging");
    create_description_label(parent, "Decide what to do with the ground coffee after the grinder is primed.");

    // Radio button group for grinder purge mode (Keep/Remove)
    const char* grinder_purge_modes[] = {"Keep", "Remove"};
    grinder_purge_mode_radio_group = create_radio_button_group(
        parent,
        grinder_purge_modes,
        2,
        LV_FLEX_FLOW_ROW,
        1,  // Purge initially selected (index 1)
        135, 100,  // Width, Height
        grinder_purge_mode_callback,
        this
    );

    create_description_label(parent, "Purge amount is a minimum target, not an exact goal.");

    // Slider for grinder purge amount (uses kPurgeSliderScale for resolution)
    const uint32_t slider_min_units = static_cast<uint32_t>(GRIND_PURGE_AMOUNT_MIN_G * kPurgeSliderScale + 0.5f);
    const uint32_t slider_max_units = static_cast<uint32_t>(GRIND_PURGE_AMOUNT_MAX_G * kPurgeSliderScale + 0.5f);
    create_slider_row(parent, "Amount", &grinder_purge_amount_label, &grinder_purge_amount_slider,
                     lv_color_hex(THEME_COLOR_ACCENT), slider_min_units, slider_max_units);

    create_description_label(parent, "Set how long grounds stay fresh before showing purge prompt.");

    // Slider for grind freshness hours (discrete steps: 0.5, 1, 2, 3, 4, 8, 12, 24, 48)
    create_slider_row(parent, "Freshness", &grind_freshness_hours_label, &grind_freshness_hours_slider,
                     lv_color_hex(THEME_COLOR_ACCENT), 0, 8);  // 9 positions (0-8)

    // Register events for the toggles (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(grind_mode_swipe_toggle, LV_EVENT_VALUE_CHANGED, ET::GRIND_MODE_SWIPE_TOGGLE);
    bind_event(auto_start_toggle, LV_EVENT_VALUE_CHANGED, ET::AUTO_START_TOGGLE);
    bind_event(auto_return_toggle, LV_EVENT_VALUE_CHANGED, ET::AUTO_RETURN_TOGGLE);
    bind_event(grinder_purge_amount_slider, LV_EVENT_VALUE_CHANGED, ET::GRINDER_PURGE_AMOUNT_SLIDER);
    bind_event(grinder_purge_amount_slider, LV_EVENT_RELEASED, ET::GRINDER_PURGE_AMOUNT_SLIDER_RELEASED);
    bind_event(grind_freshness_hours_slider, LV_EVENT_VALUE_CHANGED, ET::GRIND_FRESHNESS_HOURS_SLIDER);
    bind_event(grind_freshness_hours_slider, LV_EVENT_RELEASED, ET::GRIND_FRESHNESS_HOURS_SLIDER_RELEASED);
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

bool MenuScreen::is_info_page_current() const {
    return visible && menu && info_page && lv_menu_get_cur_main_page(menu) == info_page;
}

bool MenuScreen::is_diagnostics_page_current() const {
    return visible && menu && diagnostics_page && lv_menu_get_cur_main_page(menu) == diagnostics_page;
}

bool MenuScreen::is_ble_page_current() const {
    return visible && menu && bluetooth_page && lv_menu_get_cur_main_page(menu) == bluetooth_page;
}

void MenuScreen::update_grinder_purge_amount_label(float amount_g) {
    if (grinder_purge_amount_label) {
        char buffer[16];
        float clamped_amount = amount_g;
        if (clamped_amount < GRIND_PURGE_AMOUNT_MIN_G) clamped_amount = GRIND_PURGE_AMOUNT_MIN_G;
        if (clamped_amount > GRIND_PURGE_AMOUNT_MAX_G) clamped_amount = GRIND_PURGE_AMOUNT_MAX_G;
        snprintf(buffer, sizeof(buffer), "Amount: %.1fg", clamped_amount);
        lv_label_set_text(grinder_purge_amount_label, buffer);
    }
}

void MenuScreen::update_grind_freshness_hours_label(float hours) {
    if (grind_freshness_hours_label) {
        char buffer[24];
        if (hours < 1.0f) {
            snprintf(buffer, sizeof(buffer), "Freshness: %.1fh", hours);
        } else {
            snprintf(buffer, sizeof(buffer), "Freshness: %.0fh", hours);
        }
        lv_label_set_text(grind_freshness_hours_label, buffer);
    }
}

void MenuScreen::update_grind_mode_toggles() {
    // Read swipe enabled from "swipe" namespace
    Preferences swipe_prefs;
    swipe_prefs.begin("swipe", true); // read-only
    bool swipe_enabled = swipe_prefs.getBool("enabled", false);
    swipe_prefs.end();

    // Read current grind mode from main grinder preferences using hardware manager
    int mode_index = 0; // Default to Weight (index 0)
    int grinder_purge_mode_index = GRIND_PURGE_MODE_DEFAULT;  // Default to Purge
    float grinder_purge_amount_g = GRIND_PURGE_AMOUNT_DEFAULT_G;  // Default to 1.0g
    if (hardware_manager) {
        Preferences* main_prefs = hardware_manager->get_preferences();
        if (main_prefs) {
            int stored_mode = main_prefs->getInt("grind_mode", static_cast<int>(GrindMode::WEIGHT));
            mode_index = (stored_mode == static_cast<int>(GrindMode::TIME)) ? 1 : 0;
            grinder_purge_mode_index = main_prefs->getInt(GrindController::PREF_KEY_GRINDER_MODE, GRIND_PURGE_MODE_DEFAULT);
            grinder_purge_amount_g = main_prefs->getFloat(GrindController::PREF_KEY_GRINDER_AMOUNT_G, GRIND_PURGE_AMOUNT_DEFAULT_G);
        }
    }

    if (grind_mode_radio_group) {
        radio_button_group_set_selection(grind_mode_radio_group, mode_index);
    }

    if (grind_mode_swipe_toggle) {
        if (swipe_enabled) {
            lv_obj_add_state(grind_mode_swipe_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(grind_mode_swipe_toggle, LV_STATE_CHECKED);
        }
    }

    // Auto actions toggles (defaults disabled)
    Preferences auto_prefs;
    auto_prefs.begin("autogrind", true);
    bool auto_start_enabled = auto_prefs.getBool("auto_start", false);
    bool auto_return_enabled = auto_prefs.getBool("auto_return", false);
    auto_prefs.end();

    if (auto_start_toggle) {
        if (auto_start_enabled) {
            lv_obj_add_state(auto_start_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(auto_start_toggle, LV_STATE_CHECKED);
        }
    }

    if (auto_return_toggle) {
        if (auto_return_enabled) {
            lv_obj_add_state(auto_return_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(auto_return_toggle, LV_STATE_CHECKED);
        }
    }

    // Update grinder purge mode radio group selection
    if (grinder_purge_mode_radio_group) {
        radio_button_group_set_selection(grinder_purge_mode_radio_group, grinder_purge_mode_index);
    }

    grinder_purge_amount_g = std::clamp(grinder_purge_amount_g, GRIND_PURGE_AMOUNT_MIN_G, GRIND_PURGE_AMOUNT_MAX_G);
    const int slider_min_units = static_cast<int>(GRIND_PURGE_AMOUNT_MIN_G * kPurgeSliderScale + 0.5f);
    const int slider_max_units = static_cast<int>(GRIND_PURGE_AMOUNT_MAX_G * kPurgeSliderScale + 0.5f);

    // Update grinder purge amount slider using kPurgeSliderScale (0.1g resolution)
    if (grinder_purge_amount_slider) {
        int slider_value = static_cast<int>(grinder_purge_amount_g * kPurgeSliderScale + 0.5f);
        slider_value = std::clamp(slider_value, slider_min_units, slider_max_units);
        lv_slider_set_value(grinder_purge_amount_slider, slider_value, LV_ANIM_OFF);
    }

    // Update grinder purge amount label
    update_grinder_purge_amount_label(grinder_purge_amount_g);

    // Load and set grind freshness hours
    float freshness_hours = GRIND_FRESHNESS_DEFAULT_HOURS;
    if (hardware_manager) {
        Preferences* main_prefs = hardware_manager->get_preferences();
        if (main_prefs) {
            freshness_hours = main_prefs->getFloat(GrindController::PREF_KEY_GRIND_FRESHNESS_HOURS, GRIND_FRESHNESS_DEFAULT_HOURS);
        }
    }

    // Map hours to slider index (discrete steps: 0.5, 1, 2, 3, 4, 8, 12, 24, 48)
    static const float freshness_steps[] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 8.0f, 12.0f, 24.0f, 48.0f};
    int slider_index = 5; // Default to 8h
    for (int i = 0; i < 9; i++) {
        if (fabsf(freshness_hours - freshness_steps[i]) < 0.1f) {
            slider_index = i;
            break;
        }
    }

    if (grind_freshness_hours_slider) {
        lv_slider_set_value(grind_freshness_hours_slider, slider_index, LV_ANIM_OFF);
    }

    update_grind_freshness_hours_label(freshness_hours);
}

LV_DEPRECATIONS_IGNORE_END
