#include "ready_controller.h"

#include <lvgl.h>
#include "../../config/constants.h"
#include "../../controllers/grind_mode_traits.h"
#include "../../controllers/grind_size.h"
#include "../../system/nvs_settings_store.h"
#include "../event_bridge_lvgl.h"
#include "../ui_manager.h"

ReadyUIController::ReadyUIController(UIManager* manager)
    : ui_manager_(manager) {}

void ReadyUIController::update() {}

void ReadyUIController::refresh_profiles() {
    if (!ui_manager_ || !ui_manager_->profile_controller) {
        return;
    }

    float values[USER_PROFILE_COUNT];
    for (int i = 0; i < USER_PROFILE_COUNT; ++i) {
        values[i] = get_profile_target(*ui_manager_->profile_controller, ui_manager_->current_mode, i);
    }
    ui_manager_->ready_screen.update_profile_values(values, ui_manager_->current_mode);

    // Grind size line per profile page (hidden until a size exists). Event-driven like the values, not per frame.
    for (int i = 0; i < USER_PROFILE_COUNT; ++i) {
        char text[GRIND_SIZE_TEXT_BUF];
        grind_size_format(text, sizeof(text), ui_manager_->profile_controller->get_grind_size(i));
        ui_manager_->ready_screen.update_grind_size_text(i, text);
    }
}

// The PROFILE page exists exactly while the profile controller has a dose. When that changes the READY screen is
// rebuilt, but only where nothing is using it (docs/DESIGN-NOTES.md "GaggiMate link (chg020)"): in the menu, or on
// READY while nobody touches the screen. Anywhere else the change waits for a later frame.
void ReadyUIController::sync_pages() {
    if (!ui_manager_ || !ui_manager_->profile_controller || !ui_manager_->state_machine) {
        return;
    }

    auto& ready = ui_manager_->ready_screen;
    const bool want_dose_page = ui_manager_->profile_controller->has_dose_profile();
    if (want_dose_page == ready.has_dose_page()) {
        return;
    }

    const bool in_menu = ui_manager_->state_machine->is_state(UIState::MENU);
    const bool on_ready = ui_manager_->state_machine->is_state(UIState::READY);
    if (!in_menu && !(on_ready && lv_display_get_inactive_time(nullptr) >= READY_REBUILD_IDLE_MS)) {
        return;
    }

    const bool had_dose_page = ready.has_dose_page();
    ready.rebuild(want_dose_page);
    register_screen_events();
    ui_manager_->current_tab = ready_remap_tab(ui_manager_->current_tab, had_dose_page, want_dose_page,
                                               ui_manager_->profile_controller->get_current_profile());
    refresh_profiles();
    if (in_menu) {
        ui_manager_->menu_screen.set_dose_profile_row_visible(want_dose_page);  // a dose can appear while the menu is open
    }
    if (on_ready) {
        ready.set_active_tab(ui_manager_->current_tab);  // from the menu, showing READY does this
    }
    if (ui_manager_->grinding_controller_) {
        ui_manager_->grinding_controller_->update_grind_button_icon();
    }
}

void ReadyUIController::handle_tab_change(int tab) {
    if (!ui_manager_) {
        return;
    }

    ui_manager_->current_tab = tab;
    if (ui_manager_->profile_controller && tab < ui_manager_->menu_tab_index()) {
        ui_manager_->profile_controller->set_current_profile(tab);
        refresh_profiles();
    }

    if (ui_manager_->grinding_controller_) {
        ui_manager_->grinding_controller_->update_grind_button_icon();
    }
}

void ReadyUIController::handle_profile_long_press() {
    if (!ui_manager_ || !ui_manager_->state_machine) {
        return;
    }

    if (!ui_manager_->state_machine->is_state(UIState::READY) || ui_manager_->current_tab >= ui_manager_->menu_tab_index()) {
        return;
    }

    ui_manager_->original_target = get_current_profile_target(*ui_manager_->profile_controller, ui_manager_->current_mode);
    ui_manager_->edit_target = ui_manager_->original_target;
    ui_manager_->edit_screen.set_mode(ui_manager_->current_mode);
    if (ui_manager_->edit_controller_) {
        ui_manager_->edit_controller_->update_display();
    }
    ui_manager_->switch_to_state(UIState::EDIT);
}

void ReadyUIController::toggle_mode() {
    if (!ui_manager_ || ui_manager_->current_tab >= ui_manager_->menu_tab_index()) {
        return;
    }

    bool swipe_enabled = ui_settings.swipe_enabled();

    if (!swipe_enabled) {
        return;
    }

    const GrindMode new_mode = (ui_manager_->current_mode == GrindMode::WEIGHT) ? GrindMode::TIME : GrindMode::WEIGHT;

    // Not stored: the profile controller kept the old mode, so the UI keeps it too
    if (ui_manager_->profile_controller && !ui_manager_->profile_controller->set_grind_mode(new_mode)) {
        return;
    }
    ui_manager_->current_mode = new_mode;

    refresh_profiles();
    ui_manager_->edit_target = get_current_profile_target(*ui_manager_->profile_controller, ui_manager_->current_mode);
    if (ui_manager_->state_machine && ui_manager_->state_machine->is_state(UIState::EDIT)) {
        if (ui_manager_->edit_controller_) {
            ui_manager_->edit_controller_->update_display();
        }
    }

    ui_manager_->grinding_screen.set_mode(ui_manager_->current_mode);
    if (ui_manager_->state_machine &&
        (ui_manager_->state_machine->is_state(UIState::GRINDING) ||
         ui_manager_->state_machine->is_state(UIState::GRIND_COMPLETE))) {
        if (ui_manager_->grinding_controller_) {
            ui_manager_->grinding_controller_->update_grinding_targets();
        }
    }

    if (ui_manager_->grinding_controller_) {
        ui_manager_->grinding_controller_->update_grind_button_icon();
    }
}

// Vertical swipe on READY toggles weight/time mode
void ReadyUIController::on_gesture(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir != LV_DIR_TOP && dir != LV_DIR_BOTTOM) {
        return;
    }
    UIManager* ui = static_cast<UIManager*>(lv_event_get_user_data(e));
    if (ui && ui->state_machine->is_state(UIState::READY) && ui->ready_controller_) {
        ui->ready_controller_->toggle_mode();
    }
}

// Events on the objects of the READY screen; run again after every rebuild of it (the new objects have none)
void ReadyUIController::register_screen_events() {
    if (!ui_manager_) {
        return;
    }

    lv_obj_t* ready_screen_obj = ui_manager_->ready_screen.get_screen();
    lv_obj_t* tabview = ui_manager_->ready_screen.get_tabview();

    if (tabview) {
        lv_obj_add_event_cb(tabview, EventBridgeLVGL::dispatch_event, LV_EVENT_VALUE_CHANGED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(EventBridgeLVGL::EventType::TAB_CHANGE)));
        lv_obj_add_event_cb(tabview, on_gesture, LV_EVENT_GESTURE, ui_manager_);
    }
    if (ready_screen_obj) {
        lv_obj_add_event_cb(ready_screen_obj, on_gesture, LV_EVENT_GESTURE, ui_manager_);
    }

    ui_manager_->ready_screen.set_profile_long_press_handler(EventBridgeLVGL::profile_long_press_handler);
}

void ReadyUIController::register_events() {
    if (!ui_manager_) {
        return;
    }

    register_screen_events();

    // The active screen outlives every rebuild: its gesture handler is added once
    lv_obj_add_event_cb(lv_screen_active(), on_gesture, LV_EVENT_GESTURE, ui_manager_);

    EventBridgeLVGL::register_handler(EventBridgeLVGL::EventType::TAB_CHANGE,
                                      [this](lv_event_t* event) {
                                          lv_obj_t* tabview_obj = static_cast<lv_obj_t*>(lv_event_get_target(event));
                                          uint32_t tab_id = lv_tabview_get_tab_active(tabview_obj);
                                          handle_tab_change(static_cast<int>(tab_id));
                                      });

    EventBridgeLVGL::register_handler(EventBridgeLVGL::EventType::PROFILE_LONG_PRESS,
                                      [this](lv_event_t*) { handle_profile_long_press(); });
}
