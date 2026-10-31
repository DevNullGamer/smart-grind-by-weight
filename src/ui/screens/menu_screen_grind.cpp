// MenuScreen: Grind Settings page (mode, swipe, automation, purge, freshness).
#include "menu_screen.h"
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include "../../config/constants.h"
#include "../../hardware/hardware_manager.h"
#include "../event_bridge_lvgl.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

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
