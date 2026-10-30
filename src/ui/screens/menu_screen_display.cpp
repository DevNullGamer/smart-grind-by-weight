// MenuScreen: Display page (brightness sliders).
#include "menu_screen.h"
#include <Arduino.h>
#include <Preferences.h>
#include "../../config/constants.h"
#include "../../hardware/hardware_manager.h"
#include "../event_bridge_lvgl.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_display_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_slider_row(parent, "Brightness", &brightness_normal_label, &brightness_normal_slider);
    create_slider_row(parent, "Screensaver", &brightness_screensaver_label, &brightness_screensaver_slider, lv_color_hex(THEME_COLOR_WARNING));

    // Register events for the sliders (done here because widgets are created lazily)
    using ET = EventBridgeLVGL::EventType;
    bind_event(brightness_normal_slider, LV_EVENT_VALUE_CHANGED, ET::BRIGHTNESS_NORMAL_SLIDER);
    bind_event(brightness_normal_slider, LV_EVENT_RELEASED, ET::BRIGHTNESS_NORMAL_SLIDER_RELEASED);
    bind_event(brightness_screensaver_slider, LV_EVENT_VALUE_CHANGED, ET::BRIGHTNESS_SCREENSAVER_SLIDER);
    bind_event(brightness_screensaver_slider, LV_EVENT_RELEASED, ET::BRIGHTNESS_SCREENSAVER_SLIDER_RELEASED);
}

void MenuScreen::update_brightness_sliders() {
    if (!hardware_manager || !brightness_normal_slider || !brightness_screensaver_slider) return;
    
    // Read from the dedicated "brightness" namespace using a local Preferences
    // instance so we don't interfere with the global shared handle.
    Preferences prefs;
    prefs.begin("brightness", false); // writable so namespace is created on first use
    
    // Load brightness values from preferences (default to compile-time values)
    float normal_brightness = prefs.getFloat("normal", USER_SCREEN_BRIGHTNESS_NORMAL);
    float screensaver_brightness = prefs.getFloat("screensaver", USER_SCREEN_BRIGHTNESS_DIMMED);
    prefs.end();
    
    // Convert from 0.0-1.0 to 15-100 range
    int normal_percent = (int)(normal_brightness * 100);
    int screensaver_percent = (int)(screensaver_brightness * 100);
    
    // Ensure minimum 15%
    if (normal_percent < HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT) normal_percent = HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT;
    if (screensaver_percent < HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT) screensaver_percent = HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT;
    
    // Update sliders
    lv_slider_set_value(brightness_normal_slider, normal_percent, LV_ANIM_OFF);
    lv_slider_set_value(brightness_screensaver_slider, screensaver_percent, LV_ANIM_OFF);
    
    update_brightness_labels(normal_percent, screensaver_percent);
}

void MenuScreen::update_brightness_labels(int normal_percent, int screensaver_percent) {
    if (brightness_normal_label && normal_percent >= 0) {
        char normal_text[32];
        snprintf(normal_text, sizeof(normal_text), "Brightness: %d%%", normal_percent);
        lv_label_set_text(brightness_normal_label, normal_text);
    }

    if (brightness_screensaver_label && screensaver_percent >= 0) {
        char screensaver_text[32];
        snprintf(screensaver_text, sizeof(screensaver_text), "Dimmed: %d%%", screensaver_percent);
        lv_label_set_text(brightness_screensaver_label, screensaver_text);
    }
}

LV_DEPRECATIONS_IGNORE_END
