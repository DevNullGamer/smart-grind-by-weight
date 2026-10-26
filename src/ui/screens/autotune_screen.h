#pragma once
#include <lvgl.h>
#include "../../config/constants.h"
#include "../../controllers/autotune_controller.h"

enum class AutoTuneScreenState {
    CONSOLE,        // Running with console log
    RESULT          // Final success/failure screen
};

class AutoTuneScreen {
private:
    lv_obj_t* screen = nullptr;
    lv_obj_t* title_label = nullptr;

    // Console screen elements
    lv_obj_t* console_container = nullptr;
    lv_obj_t* console_textarea = nullptr;

    // Result screen elements
    lv_obj_t* result_container = nullptr;
    lv_obj_t* final_latency_label = nullptr;
    lv_obj_t* previous_latency_label = nullptr;
    lv_obj_t* message_label = nullptr;

    // Buttons
    lv_obj_t* button_row = nullptr;
    lv_obj_t* cancel_button = nullptr;
    lv_obj_t* ok_button = nullptr;

    bool visible = false;
    AutoTuneScreenState current_state = AutoTuneScreenState::CONSOLE;

    void apply_result_layout();

public:
    void create();
    void show();
    void hide();

    void show_console_screen();
    void show_success_screen(float new_latency_ms, float previous_latency_ms);
    void show_failure_screen(const char* error_message);

    void append_console_message(const char* message);
    void update_progress(const AutoTuneProgress& progress);

    bool is_visible() const { return visible; }
    AutoTuneScreenState get_state() const { return current_state; }
    lv_obj_t* get_screen() const { return screen; }
    lv_obj_t* get_cancel_button() const { return cancel_button; }
    lv_obj_t* get_ok_button() const { return ok_button; }
};
