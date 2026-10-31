// MenuScreen: Display page (brightness sliders, see menu_screen_settings.cpp).
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
}

LV_DEPRECATIONS_IGNORE_END
