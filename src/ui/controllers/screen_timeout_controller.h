#pragma once

#include <cstdint>

class UIManager;

// Implements automatic screen dimming based on touch/weight activity

class ScreenTimeoutController {
public:
    explicit ScreenTimeoutController(UIManager* manager);

    void register_events();
    void update();

    // While held, the screen is brought back to normal brightness and does not dim (an update prompt or progress
    // screen must stay visible). Call with false on every resolution; dimming then resumes from the last touch.
    void hold_awake(bool hold) { held_awake_ = hold; }

    // Puts the display at the stored brightness of the level it is at now (normal, or dimmed while the screen
    // saver is on). For a brightness setting changed from outside the menu (web panel); a dimmed screen stays dim.
    void reapply_brightness();

private:
    UIManager* ui_manager_;
    bool screen_dimmed_;
    // The 300 s weight-range check walks the whole sample ring under the buffer lock, so it runs at
    // most once per WEIGHT_ACTIVITY_CHECK_INTERVAL_MS and the result is cached (UI task only).
    static constexpr uint32_t WEIGHT_ACTIVITY_CHECK_INTERVAL_MS = 1000;
    bool weight_activity_valid_ = false;
    bool recent_weight_activity_ = false;
    uint32_t weight_activity_checked_ms_ = 0;
    volatile bool held_awake_ = false;
};
