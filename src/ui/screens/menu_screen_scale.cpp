// MenuScreen: Scale page.
#include "menu_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

using namespace menu_ui;

void MenuScreen::create_scale_page(lv_obj_t* parent) {
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(parent, 24, 0);
    lv_obj_set_style_pad_gap(parent, 28, 0);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);

    lv_obj_t* subtitle = lv_label_create(parent);
    lv_label_set_text(subtitle, "Live weight");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);

    scale_weight_label = lv_label_create(parent);
    lv_label_set_text(scale_weight_label, "0.0g");
    lv_obj_set_style_text_font(scale_weight_label, &lv_font_montserrat_56, 0);
    lv_obj_set_style_text_color(scale_weight_label, lv_color_hex(THEME_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(scale_weight_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, LV_PCT(100), 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    scale_tare_button = create_button(parent, "TARE", lv_color_hex(THEME_COLOR_PRIMARY), THEME_CONTENT_INNER_WIDTH_PX, 80, &lv_font_montserrat_28);
    using ET = EventBridgeLVGL::EventType;
    bind_event(scale_tare_button, LV_EVENT_CLICKED, ET::MENU_SCALE_TARE);
}

void MenuScreen::reset_scale_display() {
    if (scale_weight_label) {
        lv_label_set_text(scale_weight_label, "0.0g");
    }
}

void MenuScreen::update_scale_weight(float weight) {
    if (!scale_weight_label) {
        return;
    }
    char buffer[24];
    snprintf(buffer, sizeof(buffer), SYS_WEIGHT_DISPLAY_FORMAT, weight);
    set_label_text_if_changed(scale_weight_label, buffer);
}

LV_DEPRECATIONS_IGNORE_END
