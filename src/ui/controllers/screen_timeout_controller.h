#pragma once

#include <cstdint>
#include "../../system/screen_power.h"

class UIManager;
class DisplayManager;
class WeightSensor;

// Implements automatic screen dimming and turning off based on touch/weight activity.
// The decisions are made by the pure screen_power_decide(); this class gathers the inputs and applies the level.

class ScreenTimeoutController {
public:
    explicit ScreenTimeoutController(UIManager* manager);

    void register_events();
    void update();

    // While held, the screen is brought back to normal brightness and does not dim (an update prompt or progress
    // screen must stay visible). Call with false on every resolution; dimming then resumes from the last touch.
    void hold_awake(bool hold) { held_awake_ = hold; }

    // Same effect for the web panel's remote-start lead-in. A separate flag, because hold_awake() is a plain level
    // that the WiFi update controller sets and clears on its own edges: sharing it would let either release the other's hold.
    void hold_awake_remote_start(bool hold) { remote_start_hold_ = hold; }

    // Puts the display at the stored brightness of the level it is at now (normal, or dimmed while the screen
    // saver is on). For a brightness setting changed from outside the menu (web panel); a dimmed screen stays dim.
    void reapply_brightness();

    // Re-reads "Dim after" and "Screen off" from the settings and caches them in milliseconds (0 = never).
    // UI task only. Also called lazily on the first update(); never read per frame.
    void reload_settings();

private:
    void apply_level(DisplayManager* display, ScreenLevel level);
    void poll_weight_activity(WeightSensor* sensor, uint32_t now_ms);

    uint32_t dim_after_ms_ = 0;
    uint32_t off_after_ms_ = 0;
    bool settings_loaded_ = false;
    UIManager* ui_manager_;
    ScreenLevel level_ = ScreenLevel::AWAKE;
    // Touch input is swallowed while the screen is OFF and for the touch that woke it (see screen_power.h)
    bool input_blocked_ = false;
    // The weight-range check walks the sample ring under the buffer lock, so it runs at most once per
    // WEIGHT_ACTIVITY_CHECK_INTERVAL_MS (UI task only). One USER_WEIGHT_ACTIVITY_WINDOW_MS range feeds both
    // thresholds; the time of the last hit is remembered, because the ring (about 100 s) is shorter than the timeouts.
    static constexpr uint32_t WEIGHT_ACTIVITY_CHECK_INTERVAL_MS = 1000;
    bool weight_poll_valid_ = false;
    uint32_t weight_poll_ms_ = 0;
    bool weight_activity_seen_ = false;        // >= 1 g
    uint32_t last_weight_activity_ms_ = 0;
    bool strong_weight_activity_seen_ = false; // >= 2 g
    uint32_t last_strong_weight_activity_ms_ = 0;
    volatile bool held_awake_ = false;
    volatile bool remote_start_hold_ = false;
};
