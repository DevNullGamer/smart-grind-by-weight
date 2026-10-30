// MenuScreen: Info page.
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_info_page(lv_obj_t* parent) {
    init_column_page(parent);
    lv_obj_set_style_pad_all(parent, 0, 0);

    char build_info[64];
    snprintf(build_info, sizeof(build_info), "#%d", BUILD_NUMBER);

    create_description_label(parent, "Device metrics and current status snapshot.");

    create_static_data_label(parent, "Firmware:", "v" BUILD_FIRMWARE_VERSION);
    create_static_data_label(parent, "Build:", build_info);    
    
    create_separator(parent);
    
    create_data_label(parent, "Instant:", &instant_label);
    create_data_label(parent, "Samples:", &samples_label);
    create_data_label(parent, "Raw:", &raw_label);

    create_separator(parent);
   
    create_data_label(parent, "Uptime:", &uptime_label);
    create_data_label(parent, "RAM:", &memory_label);
}

void MenuScreen::update_info(const WeightSensor* weight_sensor, unsigned long uptime_ms, size_t free_heap) {
    if (!visible) return;

    set_label_text_float(instant_label, weight_sensor->get_instant_weight(), "g");
    set_label_text_int(samples_label, weight_sensor->get_sample_count());
    set_label_text_int(raw_label, weight_sensor->get_raw_adc_instant());

    // Update uptime - use compact format to avoid horizontal scrolling
    unsigned long seconds = uptime_ms / 1000;
    unsigned long hours = seconds / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    seconds = seconds % 60;

    char uptime_text[48];
    snprintf(uptime_text, sizeof(uptime_text), "%02lu:%02lu:%02lu", hours, minutes, seconds);
    set_label_text_if_changed(uptime_label, uptime_text);

    set_label_text_int(memory_label, free_heap / 1024, "kB");
}

LV_DEPRECATIONS_IGNORE_END
