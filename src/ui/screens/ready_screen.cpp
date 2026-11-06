#include "ready_screen.h"
#include <Arduino.h>
#include "../../config/constants.h"
#include "../../controllers/grind_mode_traits.h"
#include "../ui_helpers.h"

void ReadyScreen::create(bool with_dose_page) {
    dose_page = with_dose_page;
    for (int i = 0; i < USER_PROFILE_COUNT; i++) {
        profile_tabs[i] = nullptr;
        weight_labels[i] = nullptr;
        grind_labels[i] = nullptr;
    }

    screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(80));
    lv_obj_align(screen, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_gesture_bubble(screen, true);

    // Create tabview
    tabview = lv_tabview_create(screen);
    lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
    lv_obj_align(tabview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scroll_chain_ver(tabview, true);
    lv_obj_set_gesture_bubble(tabview, true);

    // Hide tab buttons for swipe-only interface
    lv_obj_t* tab_btns = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_hidden(tab_btns, true);

    // Transparent background
    lv_obj_set_style_bg_opa(tabview, LV_OPA_TRANSP, 0);

    // Profile pages in profile-index order, then MENU (its index is ready_menu_tab_index(dose_page))
    const float default_weights[USER_PROFILE_COUNT] = {USER_SINGLE_ESPRESSO_WEIGHT_G, USER_DOUBLE_ESPRESSO_WEIGHT_G,
                                                       USER_CUSTOM_PROFILE_WEIGHT_G, USER_DOSE_PROFILE_WEIGHT_G};
    const char* const names[USER_PROFILE_COUNT] = {"SINGLE", "DOUBLE", "CUSTOM", "PROFILE"};
    const char* const tab_names[USER_PROFILE_COUNT] = {"Single", "Double", "Custom", "Profile"};

    const int page_count = menu_tab_index();
    for (int i = 0; i < page_count; i++) {
        profile_tabs[i] = lv_tabview_add_tab(tabview, tab_names[i]);
        create_profile_page(profile_tabs[i], i, names[i], default_weights[i]);
    }

    // Create menu tab page
    menu_tab = lv_tabview_add_tab(tabview, "MENU");
    create_menu_page(menu_tab);

    update_profile_values(default_weights, GrindMode::WEIGHT);

    visible = false;
}

void ReadyScreen::rebuild(bool with_dose_page) {
    const bool was_visible = visible;
    int32_t order = 0;
    if (screen) {
        order = lv_obj_get_index(screen);
        lv_obj_delete(screen);  // takes the tabview, pages and labels with it; LVGL resets input references to them
    }
    create(with_dose_page);
    lv_obj_move_to_index(screen, order);
    if (was_visible) {
        show();
    } else {
        hide();
    }
}

void ReadyScreen::create_profile_page(lv_obj_t* parent, int profile_index, const char* profile_name, float weight) {
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(parent, 0, 0);

    lv_obj_t* name_label;
    (void)create_profile_label(parent, &name_label, &weight_labels[profile_index], &grind_labels[profile_index]);
    lv_label_set_text(name_label, profile_name);
    lv_obj_set_clickable(name_label, true);
    
    char weight_text[16];
    snprintf(weight_text, sizeof(weight_text), SYS_WEIGHT_DISPLAY_FORMAT, weight);
    lv_label_set_text(weight_labels[profile_index], weight_text);
    lv_obj_set_clickable(weight_labels[profile_index], true);
}

void ReadyScreen::create_menu_page(lv_obj_t* parent) {
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(parent, 20, 0);

    // Info label
    lv_obj_t* info_label = lv_label_create(parent);
    lv_label_set_text(info_label, "MAIN\nMENU");
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(info_label, lv_color_hex(THEME_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_CENTER, 0);
}

void ReadyScreen::show() {
    lv_obj_set_hidden(screen, false);
    visible = true;
}

void ReadyScreen::hide() {
    lv_obj_set_hidden(screen, true);
    visible = false;
}

void ReadyScreen::update_profile_values(const float values[USER_PROFILE_COUNT], GrindMode mode) {
    for (int i = 0; i < USER_PROFILE_COUNT; i++) {
        if (weight_labels[i]) {
            char text[24];
            format_ready_value(text, sizeof(text), mode, values[i]);
            set_label_text_if_changed(weight_labels[i], text);
        }
    }
}

void ReadyScreen::update_grind_size_text(int profile_index, const char* text) {
    if (profile_index < 0 || profile_index >= USER_PROFILE_COUNT || !grind_labels[profile_index]) {
        return;
    }
    lv_obj_t* label = grind_labels[profile_index];
    const bool show_line = text && text[0] != '\0';
    if (show_line) {
        set_label_text_if_changed(label, text);
    }
    if (lv_obj_is_hidden(label) == show_line) {
        lv_obj_set_hidden(label, !show_line);
    }
}

void ReadyScreen::set_active_tab(int tab) {
    if (tab >= 0 && tab <= menu_tab_index()) {
        lv_tabview_set_active(tabview, tab, LV_ANIM_OFF);
    }
}

void ReadyScreen::set_profile_long_press_handler(lv_event_cb_t handler) {
    for (int i = 0; i < USER_PROFILE_COUNT; i++) {
        if (weight_labels[i]) {
            lv_obj_add_event_cb(weight_labels[i], handler, LV_EVENT_LONG_PRESSED, NULL);
        }
    }
}
