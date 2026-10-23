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

private:
    UIManager* ui_manager_;
    bool screen_dimmed_;
    volatile bool held_awake_ = false;
};
