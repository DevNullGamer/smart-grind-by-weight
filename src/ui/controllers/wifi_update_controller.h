#pragma once

#include <stdint.h>

class UIManager;
enum class UIState;

// UI side of WiFi updates, polled from UIManager::update() on the UI task (the HTTP task never touches LVGL):
//  - Accept dialog: shows the confirm screen for a pending update request (INSTALL / DECLINE), only from READY or
//    MENU; otherwise answers the request with "UI busy". Dismisses the dialog when the request expires or is
//    cancelled. Keeps the screen awake while a request is pending or the dialog is up.
//  - Progress screen: shows the OTA screen ("WiFi update") while UpdateStatus reports an update, a failure for
//    UPDATE_UI_FAILED_HOLD_MS, then returns to READY / MENU.
// Decisions and texts are pure functions in network/update_ui_logic.*.
class WifiUpdateUIController {
public:
    explicit WifiUpdateUIController(UIManager* manager);
    ~WifiUpdateUIController();

    // Returns true when the progress screen consumed the frame (skip further updates)
    bool update();

private:
    void update_prompt();
    bool update_progress();
    void answer_prompt(bool accept, uint32_t generation);
    void begin_progress();
    void end_progress();
    void sync_hold();

    UIManager* ui_manager_;
    bool ui_attached_ = false;

    // Accept dialog
    bool dialog_shown_ = false;
    uint32_t dialog_generation_ = 0;  // UpdateApprovalGate::generation() of the request the dialog belongs to
    bool prompt_pending_ = false;     // a request waited for an answer in the last frame

    // Progress screen
    bool progress_shown_ = false;
    UIState return_state_;
    uint32_t failed_since_ms_ = 0;
    int last_percent_ = -1;
    char last_status_[64] = {0};

    bool hold_active_ = false;
};
