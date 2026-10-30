#include "menu_widgets.h"

// lv_menu is deprecated since LVGL 9.6 (removed in v10); replacing it is tracked separately
// (chg006 item 23). All deprecation warnings are suppressed in this file, not only lv_menu ones.
LV_DEPRECATIONS_IGNORE_BEGIN

namespace menu_ui {

lv_obj_t* create_separator(lv_obj_t* parent, const char* text) {
    // Create separator container
    lv_obj_t* separator_container = lv_obj_create(parent);
    lv_obj_set_size(separator_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(separator_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(separator_container, 0, 0);
    lv_obj_set_layout(separator_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(separator_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(separator_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(separator_container, false);

    // Create left line
    lv_obj_t* left_line = lv_obj_create(separator_container);
    lv_obj_set_size(left_line, LV_SIZE_CONTENT, 2);
    lv_obj_set_flex_grow(left_line, 1);
    lv_obj_set_style_bg_color(left_line, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_border_width(left_line, 0, 0);

    if (!text) {
        // If no text, make the line take full width
        return separator_container;
    }

    // Create text label
    lv_obj_t* separator_label = lv_label_create(separator_container);
    lv_label_set_text(separator_label, text);
    lv_obj_set_style_text_font(separator_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(separator_label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_left(separator_label, 10, 0);
    lv_obj_set_style_pad_right(separator_label, 10, 0);

    // Create right line
    lv_obj_t* right_line = lv_obj_create(separator_container);
    lv_obj_set_size(right_line, LV_SIZE_CONTENT, 2);
    lv_obj_set_flex_grow(right_line, 1);
    lv_obj_set_style_bg_color(right_line, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_border_width(right_line, 0, 0);

    return separator_container;
}

lv_obj_t* create_menu_item(lv_obj_t* parent, const char* text) {
    lv_obj_t* cont = lv_menu_cont_create(parent);
    style_as_button(cont);
    lv_obj_set_style_margin_bottom(cont, 10, 0);

    // Set layout
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = lv_label_create(cont);
    lv_label_set_text(label, text);

    lv_obj_t* chevron = lv_label_create(cont);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, lv_color_hex(THEME_COLOR_SECONDARY), 0);

    return cont;
}

lv_obj_t* create_toggle_row(lv_obj_t* parent, const char* text, lv_obj_t** out_toggle) {
    lv_obj_t* row_container = lv_obj_create(parent);
    style_as_button(row_container);
    lv_obj_set_style_margin_bottom(row_container, 10, 0);

    // Set layout
    lv_obj_set_layout(row_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = lv_label_create(row_container);
    lv_label_set_text(label, text);

    *out_toggle = lv_switch_create(row_container);
    lv_obj_set_size(*out_toggle, 80, 40);
    lv_obj_set_ext_click_area(*out_toggle, 20);

    return row_container;
}

lv_obj_t* create_slider_row(lv_obj_t* parent, const char* text, lv_obj_t** label, lv_obj_t** slider,
                            lv_color_t slider_color, uint32_t min, uint32_t max) {
    lv_obj_t* row_container = lv_obj_create(parent);
    style_as_button(row_container, THEME_CONTENT_INNER_WIDTH_PX, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_bottom(row_container, 10, 0);
    // Set layout

    lv_obj_set_layout(row_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_container, 14, 0);
    lv_obj_set_style_pad_all(row_container, 20, 0);

    *label = lv_label_create(row_container);
    lv_label_set_text(*label, text);

    *slider = lv_slider_create(row_container);
    lv_obj_set_size(*slider, 220, 40);
    lv_obj_set_ext_click_area(*slider, 20);
    lv_slider_set_range(*slider, min, max);
    lv_obj_set_style_bg_color(*slider, lv_color_hex(THEME_COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_color(*slider, slider_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(*slider, LV_OPA_TRANSP, LV_PART_KNOB);
    return row_container;
}

lv_obj_t* create_static_data_label(lv_obj_t* parent, const char* name, const char* value) {
    lv_obj_t* label;
    lv_obj_t* data_label = ::create_data_label(parent, name, &label);
    lv_label_set_text(label, value);
    return data_label;
}

lv_obj_t* create_description_label(lv_obj_t* parent, const char* text) {
    // Create container with padding (similar to create_data_label)
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_pad_left(container, 10, 0);
    lv_obj_set_style_pad_right(container, 14, 0);
    lv_obj_set_style_margin_top(container, 12, 0);
    lv_obj_set_style_margin_bottom(container, 12, 0);
    lv_obj_set_size(container, THEME_CONTENT_WIDTH_PX, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(container, false);

    // Create label inside container
    lv_obj_t* label = lv_label_create(container);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_COLOR_TEXT_SECONDARY), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_PCT(100));

    return label;
}

lv_obj_t* create_block(lv_obj_t* parent) {
    lv_obj_t* block = lv_obj_create(parent);
    lv_obj_set_size(block, THEME_CONTENT_WIDTH_PX, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_gap(block, 0, 0);
    lv_obj_set_layout(block, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(block, false);
    return block;
}

void wrap_button_text(lv_obj_t* button) {
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (!label) return;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, THEME_CONTENT_INNER_WIDTH_PX - 2 * 20);  // button width minus 2 x 20 px padding
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

void set_enabled(lv_obj_t* obj, bool enabled) {
    if (!obj) return;
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

void set_visible(lv_obj_t* obj, bool show) {
    if (!obj) return;
    if (show) {
        lv_obj_set_hidden(obj, false);
    } else {
        lv_obj_set_hidden(obj, true);
    }
}

void init_column_page(lv_obj_t* parent) {
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
}

void bind_event(lv_obj_t* obj, lv_event_code_t code, EventBridgeLVGL::EventType type) {
    if (!obj) return;
    lv_obj_add_event_cb(obj, EventBridgeLVGL::dispatch_event, code,
                        reinterpret_cast<void*>(static_cast<intptr_t>(type)));
}

}  // namespace menu_ui

LV_DEPRECATIONS_IGNORE_END
