// MenuScreen: Grind Settings page (mode, swipe, automation, purge, freshness).
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../../system/menu_setting_models.h"
#include "../../system/nvs_settings_store.h"
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
    create_setting_toggle_row(parent, ToggleSetting::SWIPE);

    // Automatic actions section
    create_separator(parent, "Automation");
    create_description_label(parent, "Start the selected profile as soon as the cup lands on the scale.");
    create_setting_toggle_row(parent, ToggleSetting::AUTO_START);
    create_description_label(parent, "Exit the completion screen once that cup weight drops away.");
    create_setting_toggle_row(parent, ToggleSetting::AUTO_RETURN);

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
    const uint32_t slider_min_units = static_cast<uint32_t>(purge_slider_min());
    const uint32_t slider_max_units = static_cast<uint32_t>(purge_slider_max());
    create_slider_row(parent, "Amount", &grinder_purge_amount_label, &grinder_purge_amount_slider,
                     lv_color_hex(THEME_COLOR_ACCENT), slider_min_units, slider_max_units);

    create_description_label(parent, "Set how long grounds stay fresh before showing purge prompt.");

    // Slider for grind freshness hours (discrete steps: 0.5, 1, 2, 3, 4, 8, 12, 24, 48)
    create_slider_row(parent, "Freshness", &grind_freshness_hours_label, &grind_freshness_hours_slider,
                     lv_color_hex(THEME_COLOR_ACCENT), 0, 8);  // 9 positions (0-8)

    // Register events for the sliders (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(grinder_purge_amount_slider, LV_EVENT_VALUE_CHANGED, ET::GRINDER_PURGE_AMOUNT_SLIDER);
    bind_event(grinder_purge_amount_slider, LV_EVENT_RELEASED, ET::GRINDER_PURGE_AMOUNT_SLIDER_RELEASED);
    bind_event(grind_freshness_hours_slider, LV_EVENT_VALUE_CHANGED, ET::GRIND_FRESHNESS_HOURS_SLIDER);
    bind_event(grind_freshness_hours_slider, LV_EVENT_RELEASED, ET::GRIND_FRESHNESS_HOURS_SLIDER_RELEASED);
}

void MenuScreen::update_grinder_purge_amount_label(float amount_g) {
    if (grinder_purge_amount_label) {
        char buffer[16];
        lv_label_set_text(grinder_purge_amount_label, purge_label(buffer, sizeof(buffer), amount_g));
    }
}

void MenuScreen::update_grind_freshness_hours_label(float hours) {
    if (grind_freshness_hours_label) {
        char buffer[24];
        lv_label_set_text(grind_freshness_hours_label, freshness_label(buffer, sizeof(buffer), hours));
    }
}

void MenuScreen::update_grind_mode_toggles() {
    if (grind_mode_radio_group) {
        radio_button_group_set_selection(grind_mode_radio_group, grind_mode_radio_index(ui_settings.grind_mode_raw()));
    }

    // Update grinder purge mode radio group selection
    if (grinder_purge_mode_radio_group) {
        radio_button_group_set_selection(grinder_purge_mode_radio_group, ui_settings.purge_mode());
    }

    // Purge amount slider (0.1g resolution) and label
    const float purge_amount_g = ui_settings.purge_amount_g();
    if (grinder_purge_amount_slider) {
        lv_slider_set_value(grinder_purge_amount_slider, purge_slider_from_amount(purge_amount_g), LV_ANIM_OFF);
    }
    update_grinder_purge_amount_label(purge_amount_g);

    // Freshness slider index from the stored hours (label shows the stored value itself)
    const float freshness_hours = ui_settings.freshness_hours();
    if (grind_freshness_hours_slider) {
        lv_slider_set_value(grind_freshness_hours_slider, freshness_index_from_hours(freshness_hours), LV_ANIM_OFF);
    }
    update_grind_freshness_hours_label(freshness_hours);
}

LV_DEPRECATIONS_IGNORE_END
