#include "screen_timeout_controller.h"

#include "../../config/constants.h"
#include "../../hardware/display_manager.h"
#include "../../hardware/hardware_manager.h"
#include "../../system/nvs_settings_store.h"
#include "../../system/screen_power.h"
#include "../ui_manager.h"

ScreenTimeoutController::ScreenTimeoutController(UIManager* manager)
    : ui_manager_(manager), screen_dimmed_(false) {}

void ScreenTimeoutController::register_events() {}

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

    if (held_awake_ || remote_start_hold_ ||
        (ui_manager_->state_machine && ui_manager_->state_machine->is_state(UIState::GRINDING))) {
        if (screen_dimmed_) {
            float normal = USER_SCREEN_BRIGHTNESS_NORMAL;
            if (ui_manager_->menu_controller_) {
                normal = ui_manager_->menu_controller_->get_normal_brightness();
            }
            display->set_brightness(normal);
            screen_dimmed_ = false;
        }
        // The cached activity flag is not refreshed while held/grinding: evaluate afresh on return
        weight_activity_valid_ = false;
        return;
    }

    uint32_t ms_since_touch = touch_driver->get_ms_since_last_touch();
    auto* sensor = hardware->get_weight_sensor();
    // Re-evaluate on first use and then at most once per interval (unsigned subtraction is
    // millis()-rollover safe); a lag of up to the interval in noticing weight activity is accepted
    uint32_t now_ms = millis();
    if (!weight_activity_valid_ ||
        (uint32_t)(now_ms - weight_activity_checked_ms_) >= WEIGHT_ACTIVITY_CHECK_INTERVAL_MS) {
        recent_weight_activity_ = sensor &&
                                  sensor->weight_range_exceeds(USER_SCREEN_AUTO_DIM_TIMEOUT_MS,
                                                               USER_WEIGHT_ACTIVITY_THRESHOLD_G);
        weight_activity_checked_ms_ = now_ms;
        weight_activity_valid_ = true;
    }
    bool recent_weight_activity = recent_weight_activity_;

    bool should_dim = (ms_since_touch >= USER_SCREEN_AUTO_DIM_TIMEOUT_MS) && !recent_weight_activity;

    if (should_dim && !screen_dimmed_) {
        float dimmed = USER_SCREEN_BRIGHTNESS_DIMMED;
        if (ui_manager_->menu_controller_) {
            dimmed = ui_manager_->menu_controller_->get_screensaver_brightness();
        }
        display->set_brightness(dimmed);
        screen_dimmed_ = true;
    } else if (!should_dim && screen_dimmed_) {
        float normal = USER_SCREEN_BRIGHTNESS_NORMAL;
        if (ui_manager_->menu_controller_) {
            normal = ui_manager_->menu_controller_->get_normal_brightness();
        }
        display->set_brightness(normal);
        screen_dimmed_ = false;
    }
}

void ScreenTimeoutController::reload_settings() {
    dim_after_ms_ = screen_minutes_to_ms(screen_minutes_normalise_storage(
        ui_settings.screen_dim_minutes(), kDimMinutesSteps, kDimMinutesStepCount, USER_SCREEN_DIM_DEFAULT_MIN));
    off_after_ms_ = screen_minutes_to_ms(screen_minutes_normalise_storage(
        ui_settings.screen_off_minutes(), kOffMinutesSteps, kOffMinutesStepCount, USER_SCREEN_OFF_DEFAULT_MIN));
}

void ScreenTimeoutController::reapply_brightness() {
    if (!ui_manager_ || !ui_manager_->hardware_manager || !ui_manager_->menu_controller_) {
        return;
    }
    auto* display = ui_manager_->hardware_manager->get_display();
    if (!display) {
        return;
    }
    display->set_brightness(screen_dimmed_ ? ui_manager_->menu_controller_->get_screensaver_brightness()
                                           : ui_manager_->menu_controller_->get_normal_brightness());
}
