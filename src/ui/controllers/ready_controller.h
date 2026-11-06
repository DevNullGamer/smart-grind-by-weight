#pragma once
#include <lvgl.h>
#include "../event_bridge_lvgl.h"

class UIManager;

// Handles profile tab navigation, long-press editing, and swipe mode switching

class ReadyUIController {
public:
    explicit ReadyUIController(UIManager* manager);

    void register_events();
    void update();
    // Registers the events of the objects of the READY screen (again after each rebuild) and the one-time ones
    void register_screen_events();
    void refresh_profiles();
    // Rebuilds the READY screen when the PROFILE page appeared or disappeared (called every frame; cheap when unchanged)
    void sync_pages();
    void handle_tab_change(int tab);
    void handle_profile_long_press();
    void toggle_mode();

private:
    static void on_gesture(lv_event_t* e);  // vertical swipe on READY toggles weight/time mode
    UIManager* ui_manager_;
};
