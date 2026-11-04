// MenuScreen: Display page (brightness and screen timeout sliders, see menu_screen_settings.cpp).
#include "menu_screen.h"
#include "../../config/constants.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_display_page(lv_obj_t* parent) {
    init_column_page(parent);

    create_setting_slider_row(parent, SliderSetting::BRIGHTNESS_NORMAL);
    create_setting_slider_row(parent, SliderSetting::BRIGHTNESS_SCREENSAVER);

    create_description_label(parent, "Dim and turn off the screen when nothing is touched or weighed. "
                                     "Touch the screen or put a cup on the scale to wake it.");
    create_setting_slider_row(parent, SliderSetting::SCREEN_DIM_TIMEOUT);
    create_setting_slider_row(parent, SliderSetting::SCREEN_OFF_TIMEOUT);
}

LV_DEPRECATIONS_IGNORE_END
