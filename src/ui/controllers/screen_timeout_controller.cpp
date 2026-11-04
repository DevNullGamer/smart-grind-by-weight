#include "screen_timeout_controller.h"

#include "../../config/constants.h"
#include "../../hardware/display_manager.h"
#include "../../hardware/hardware_manager.h"
#include "../../system/nvs_settings_store.h"
#include "../../system/screen_power.h"
#include "../ui_manager.h"

namespace {
// Long-lived advisory diagnostics that must not disable burn-in protection (they would on any uncalibrated
// or noisy-bench unit). Every other active diagnostic keeps the screen on (dim still applies).
constexpr uint32_t kAdvisoryDiagnosticMask = (1u << static_cast<int>(DiagnosticCode::LOAD_CELL_NOT_CALIBRATED)) |
                                             (1u << static_cast<int>(DiagnosticCode::LOAD_CELL_NOISY_SUSTAINED));

// Milliseconds since an event stamped at stamp_ms (unsigned subtraction is millis()-rollover safe)
uint32_t ms_since(bool seen, uint32_t stamp_ms, uint32_t now_ms) {
    return seen ? static_cast<uint32_t>(now_ms - stamp_ms) : UINT32_MAX;
}
}  // namespace

ScreenTimeoutController::ScreenTimeoutController(UIManager* manager) : ui_manager_(manager) {}

void ScreenTimeoutController::register_events() {}

void ScreenTimeoutController::apply_level(DisplayManager* display, ScreenLevel level) {
    float brightness = 0.0f;
    if (level == ScreenLevel::AWAKE) {
        brightness = ui_manager_->menu_controller_ ? ui_manager_->menu_controller_->get_normal_brightness()
                                                   : USER_SCREEN_BRIGHTNESS_NORMAL;
    } else if (level == ScreenLevel::DIMMED) {
        brightness = ui_manager_->menu_controller_ ? ui_manager_->menu_controller_->get_screensaver_brightness()
                                                   : USER_SCREEN_BRIGHTNESS_DIMMED;
    }
    display->set_brightness(brightness);
}

void ScreenTimeoutController::poll_weight_activity(WeightSensor* sensor, uint32_t now_ms) {
    // Re-evaluate on first use and then at most once per interval; a lag of up to the interval in
    // noticing weight activity is accepted
    if (weight_poll_valid_ && (uint32_t)(now_ms - weight_poll_ms_) < WEIGHT_ACTIVITY_CHECK_INTERVAL_MS) {
        return;
    }
    weight_poll_ms_ = now_ms;
    weight_poll_valid_ = true;
    if (!sensor) {
        return;
    }
    const float range_g = sensor->get_weight_range(USER_WEIGHT_ACTIVITY_WINDOW_MS);
    if (range_g >= USER_WEIGHT_ACTIVITY_THRESHOLD_G) {
        weight_activity_seen_ = true;
        last_weight_activity_ms_ = now_ms;
    }
    if (range_g >= USER_SCREEN_WAKE_FROM_OFF_THRESHOLD_G) {
        strong_weight_activity_seen_ = true;
        last_strong_weight_activity_ms_ = now_ms;
    }
}

void ScreenTimeoutController::update() {
    if (!ui_manager_) {
        return;
    }

    auto* hardware = ui_manager_->hardware_manager;
    if (!hardware) {
        return;
    }

    auto* display = hardware->get_display();
    if (!display) {
        return;
    }

    auto* touch_driver = display->get_touch_driver();
    if (!touch_driver) {
        return;
    }

    if (!settings_loaded_) {
        reload_settings();
    }

    const uint32_t now_ms = millis();
    const UIState state = ui_manager_->state_machine ? ui_manager_->state_machine->get_current_state()
                                                     : UIState::GRINDING;  // unknown state: stay lit
    const bool grinding = ui_manager_->state_machine && state == UIState::GRINDING;

    poll_weight_activity(hardware->get_weight_sensor(), now_ms);
    if (grinding) {
        // A grind is activity: the screen then stays on for the full dim time after it, as before
        weight_activity_seen_ = strong_weight_activity_seen_ = true;
        last_weight_activity_ms_ = last_strong_weight_activity_ms_ = now_ms;
    }

    const bool motor_test = ui_manager_->menu_controller_ && ui_manager_->menu_controller_->is_motor_test_active();
    const uint32_t diag_mask =
        ui_manager_->diagnostics_controller_ ? ui_manager_->diagnostics_controller_->active_mask() : 0;

    ScreenPowerInput in;
    in.ms_since_touch = touch_driver->get_ms_since_last_touch();
    in.ms_since_weight_activity = ms_since(weight_activity_seen_, last_weight_activity_ms_, now_ms);
    in.ms_since_strong_weight_activity = ms_since(strong_weight_activity_seen_, last_strong_weight_activity_ms_, now_ms);
    in.dim_after_ms = dim_after_ms_;
    in.off_after_ms = off_after_ms_;
    in.keep_awake = held_awake_ || remote_start_hold_ || grinding || motor_test;
    // The BLE provisioning window is open while the WiFi page is on screen: never dark then
    in.keep_on = !ui_manager_->state_machine || !screen_state_allows_off(state) ||
                 ui_manager_->menu_screen.is_wifi_page_current() ||
                 screen_error_blocks_off(diag_mask, kAdvisoryDiagnosticMask);
    in.current = level_;

    const ScreenLevel next = screen_power_decide(in);
    if (next != level_) {
        apply_level(display, next);
    }
    // The input block follows the level in the same frame; DisplayManager::update() (touch poll and LVGL
    // read) runs after this controller, so a touch that wakes the screen is already swallowed when LVGL sees it
    input_blocked_ = screen_input_block_next(input_blocked_, level_, next, touch_driver->is_pressed());
    display->set_input_blocked(input_blocked_);
    level_ = next;
}

void ScreenTimeoutController::reload_settings() {
    dim_after_ms_ = screen_minutes_to_ms(screen_minutes_normalise_storage(
        ui_settings.screen_dim_minutes(), kDimMinutesSteps, kDimMinutesStepCount, USER_SCREEN_DIM_DEFAULT_MIN));
    off_after_ms_ = screen_minutes_to_ms(screen_minutes_normalise_storage(
        ui_settings.screen_off_minutes(), kOffMinutesSteps, kOffMinutesStepCount, USER_SCREEN_OFF_DEFAULT_MIN));
    settings_loaded_ = true;
}

void ScreenTimeoutController::reapply_brightness() {
    if (!ui_manager_ || !ui_manager_->hardware_manager || !ui_manager_->menu_controller_) {
        return;
    }
    auto* display = ui_manager_->hardware_manager->get_display();
    if (!display) {
        return;
    }
    // A screen that is off stays dark; the new brightness is used when it wakes
    if (level_ == ScreenLevel::OFF) {
        return;
    }
    apply_level(display, level_);
}
