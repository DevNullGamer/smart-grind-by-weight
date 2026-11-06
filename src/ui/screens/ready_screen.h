#pragma once
#include <lvgl.h>
#include "../../config/constants.h"
#include "../../controllers/grind_mode.h"
#include "../ready_tabs.h"

class ReadyScreen {
private:
    lv_obj_t* screen = nullptr;
    lv_obj_t* tabview = nullptr;
    lv_obj_t* profile_tabs[USER_PROFILE_COUNT] = {};
    lv_obj_t* weight_labels[USER_PROFILE_COUNT] = {};
    lv_obj_t* grind_labels[USER_PROFILE_COUNT] = {};  // grind size line under the weight, hidden until a size exists
    lv_obj_t* menu_tab = nullptr;
    bool visible = false;
    bool dose_page = false;

public:
    // dose_page: add the PROFILE page (the GaggiMate Profile Dose) before MENU
    void create(bool with_dose_page);
    // Deletes the screen and creates it again with or without the PROFILE page, keeping its place in the object
    // order (the controls created later stay on top) and its visibility. The caller re-attaches events, refreshes the
    // values and restores the active page.
    void rebuild(bool with_dose_page);
    void show();
    void hide();
    void update_profile_values(const float values[USER_PROFILE_COUNT], GrindMode mode);
    // Grind size line of a profile page: empty or null text hides it
    void update_grind_size_text(int profile_index, const char* text);
    void set_active_tab(int tab);
    void set_profile_long_press_handler(lv_event_cb_t handler);

    bool is_visible() const { return visible; }
    bool has_dose_page() const { return dose_page; }
    int menu_tab_index() const { return ready_menu_tab_index(dose_page); }
    lv_obj_t* get_screen() const { return screen; }
    lv_obj_t* get_tabview() const { return tabview; }
    lv_obj_t* get_menu_tab() const { return menu_tab; }

private:
    void create_profile_page(lv_obj_t* parent, int profile_index, const char* profile_name, float weight);
    void create_menu_page(lv_obj_t* parent);
};
