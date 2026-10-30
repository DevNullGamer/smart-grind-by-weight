#pragma once
// Stateless LVGL widget helpers shared by the menu pages (MenuScreen and menu_screen_wifi.cpp).
#include <lvgl.h>
#include "../../config/constants.h"
#include "../event_bridge_lvgl.h"
#include "../ui_helpers.h"

namespace menu_ui {

// Horizontal rule, with a centred caption when text is given
lv_obj_t* create_separator(lv_obj_t* parent, const char* text = nullptr);

// Tappable row of the main menu page (label + chevron)
lv_obj_t* create_menu_item(lv_obj_t* parent, const char* text);

// Row with a label and a switch; the switch is returned through out_toggle
lv_obj_t* create_toggle_row(lv_obj_t* parent, const char* text, lv_obj_t** out_toggle);

// Row with a label and a slider; label and slider are returned through the out pointers
lv_obj_t* create_slider_row(lv_obj_t* parent, const char* text,
                            lv_obj_t** label, lv_obj_t** slider,
                            lv_color_t slider_color = lv_color_hex(THEME_COLOR_ACCENT),
                            uint32_t min = 0, uint32_t max = 100);

lv_obj_t* create_static_data_label(lv_obj_t* parent, const char* name, const char* value);

lv_obj_t* create_description_label(lv_obj_t* parent, const char* text);

// Transparent full-width column that groups the widgets shown/hidden together
lv_obj_t* create_block(lv_obj_t* parent);

// Wraps the button text inside the button (a long "Install v1.5.0-rc.3" breaks into two centred lines)
void wrap_button_text(lv_obj_t* button);

// Null-safe DISABLED state / hidden flag setters
void set_enabled(lv_obj_t* obj, bool enabled);
void set_visible(lv_obj_t* obj, bool show);

// Flex column layout with vertical scrolling shared by the menu sub-pages
void init_column_page(lv_obj_t* parent);

// Null-safe: routes the LVGL event `code` of obj to EventBridgeLVGL (user_data = event type)
void bind_event(lv_obj_t* obj, lv_event_code_t code, EventBridgeLVGL::EventType type);

}  // namespace menu_ui
