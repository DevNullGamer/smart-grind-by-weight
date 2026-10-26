#pragma once
#include <lvgl.h>
#include "../../config/constants.h"
#include "grinding_screen_base.h"

class GrindingScreenArc : public IGrindingScreen {
private:
    lv_obj_t* screen;
    lv_obj_t* profile_label;
    lv_obj_t* target_label;
    lv_obj_t* weight_label;
    lv_obj_t* progress_arc;
    bool visible;
    bool time_mode;

    // Stored label state: updates arriving while this layout is hidden are kept here and
    // applied in show(); unchanged values never touch LVGL.
    char profile_text[48] = "DOUBLE";
    char target_text[48] = "Target: 18.0g";
    char weight_text[16] = "0.0g";
    int last_progress = 0;

    void store_text(char* dest, size_t capacity, lv_obj_t* label, const char* text);

public:
    void create() override;
    void show() override;
    void hide() override;
    void update_profile_name(const char* name) override;
    void update_target_weight(float weight) override;
    void update_target_weight_text(const char* text) override;
    void update_target_time(float seconds);
    void update_current_weight(float weight) override;
    void update_tare_display() override;
    void update_progress(int percent) override;
    void set_time_mode(bool enabled);
    
    bool is_visible() const override { return visible; }
    lv_obj_t* get_screen() const override { return screen; }
};
