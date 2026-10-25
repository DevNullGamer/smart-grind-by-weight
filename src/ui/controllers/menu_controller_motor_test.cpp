// MenuUIController: Menu -> Motor Test. Kept apart from menu_controller.cpp for size.
//
// Flow: RUN (confirm dialog) -> run_motor_test() starts the output pulse, shows the "MOTOR RUNNING"
// overlay with a STOP button and polls every HW_MOTOR_TEST_POLL_MS -> finish_motor_test() hides the
// overlay and shows a result notice. The firmware cannot see the motor, so the result only states
// what the output did (see motor_test_check.h).
//
// Rules: nothing here may open a dialog from inside the confirm callback (the confirm controller
// switches back to the previous state right after it, which would clobber a dialog opened there);
// refusals found in the callback go through UIManager::show_notice_after_dialog. Every way out of a
// running test goes through finish_motor_test(), which always deletes the timer, hides the overlay
// and clears the background indicator.
#include "menu_controller.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include "../../config/constants.h"
#include "../../system/statistics_manager.h"
#include "../components/blocking_overlay.h"
#include "../ui_manager.h"

namespace {

// Remaining time as "<seconds>.<tenth>" for the overlay, rounded up so it never shows 0.0 while running
void format_remaining(uint32_t elapsed_ms, char* out, size_t len) {
    const uint32_t remaining_ms = elapsed_ms >= HW_MOTOR_TEST_DURATION_MS ? 0 : HW_MOTOR_TEST_DURATION_MS - elapsed_ms;
    const unsigned long tenths = (static_cast<unsigned long>(remaining_ms) + 99UL) / 100UL;
    snprintf(out, len, "MOTOR RUNNING\n%lu.%lu s", tenths / 10UL, tenths % 10UL);
}

}  // namespace

void MenuUIController::handle_motor_test() {
    if (!ui_manager_) return;

    const MotorTestBlock block = ui_manager_->motor_pulse_block();
    if (block != MotorTestBlock::NONE) {
        ui_manager_->show_motor_pulse_blocked_notice(block);
        return;
    }

    ui_manager_->show_confirmation(
        "MOTOR TEST",
        "Motor will be engaged for 1 second."
        "\n\n"
        "Make sure grinder is safe to run.",
        "RUN",
        lv_color_hex(THEME_COLOR_SUCCESS),
        [this]() { run_motor_test(); },
        "CANCEL",
        [this]() { return_to_menu(); }
    );
}

// Runs inside the confirm dialog callback: no dialog may be opened here.
void MenuUIController::run_motor_test() {
    if (!ui_manager_ || motor_timer_) return;

    auto* hardware = ui_manager_->get_hardware_manager();
    auto* grinder = hardware ? hardware->get_grinder() : nullptr;
    if (!grinder) return;

    // Re-check: a firmware update may have locked the grinder out while the dialog was open
    MotorTestBlock block = ui_manager_->motor_pulse_block();
    if (block == MotorTestBlock::NONE && !grinder->start_pulse_rmt(HW_MOTOR_TEST_DURATION_MS)) {
        block = MotorTestBlock::START_FAILED;
    }
    if (block != MotorTestBlock::NONE) {
        LOG_BLE("[MotorTest] Refused - %s\n", motor_test_block_reason(block));
        auto* ui = ui_manager_;
        ui->show_notice_after_dialog([ui, block]() { ui->show_motor_pulse_blocked_notice(block); });
        return;
    }

    motor_test_start_ms_ = millis();
    ui_manager_->set_background_active(true);

    char text[40];
    format_remaining(0, text, sizeof(text));
    BlockingOperationOverlay::getInstance().show_with_stop(text, [this]() {
        if (motor_timer_) {
            finish_motor_test(false, true);
        }
    });

    motor_timer_ = lv_timer_create(static_motor_timer_cb, HW_MOTOR_TEST_POLL_MS, this);
    if (!motor_timer_) {
        // Cannot poll for the end: do not leave the output running unobserved
        LOG_BLE("[MotorTest] ERROR: could not create the poll timer, stopping\n");
        finish_motor_test(false, true);
        auto* ui = ui_manager_;
        ui->show_notice_after_dialog(
            [ui]() { ui->show_motor_pulse_blocked_notice(MotorTestBlock::START_FAILED); });
    }
}

// Every way out of a running test. completed: the output went low by itself. A test that is not
// complete is stopped here (user stop or safety stop).
void MenuUIController::finish_motor_test(bool completed, bool stopped_by_user) {
    const uint32_t elapsed_ms = millis() - motor_test_start_ms_;

    stop_motor_timer();
    BlockingOperationOverlay::getInstance().hide();

    if (!ui_manager_) return;

    auto* hardware = ui_manager_->get_hardware_manager();
    auto* grinder = hardware ? hardware->get_grinder() : nullptr;
    if (!completed && grinder) {
        grinder->stop();
    }
    ui_manager_->set_background_active(false);

    // A completed pulse ran exactly its payload; the rest of elapsed_ms is poll latency
    const uint32_t ran_ms = std::min<uint32_t>(
        elapsed_ms, completed ? HW_MOTOR_TEST_DURATION_MS : HW_MOTOR_TEST_DURATION_MS + HW_MOTOR_TEST_STOP_GRACE_MS);
    const MotorTestOutcome outcome = motor_test_finish(HW_MOTOR_TEST_DURATION_MS, ran_ms, completed, stopped_by_user);

    statistics_manager.update_motor_test(ran_ms);
    LOG_BLE("[MotorTest] Finished: outcome=%d ran=%lu ms\n", static_cast<int>(outcome), static_cast<unsigned long>(ran_ms));

    show_motor_test_result(outcome, ran_ms);
}

// Called from the poll timer or the STOP button, never from a confirm callback. Skipped when the
// menu is no longer the current state (something else took the screen).
void MenuUIController::show_motor_test_result(MotorTestOutcome outcome, uint32_t ran_ms) {
    if (!ui_manager_ || !ui_manager_->state_machine || !ui_manager_->state_machine->is_state(UIState::MENU)) {
        return;
    }

    char message[128];
    motor_test_outcome_message(outcome, HW_MOTOR_TEST_DURATION_MS, ran_ms, message, sizeof(message));

    uint32_t color = THEME_COLOR_SUCCESS;
    switch (motor_test_outcome_severity(outcome)) {
        case MotorTestSeverity::SUCCESS: color = THEME_COLOR_SUCCESS; break;
        case MotorTestSeverity::WARNING: color = THEME_COLOR_WARNING; break;
        case MotorTestSeverity::ERROR: color = THEME_COLOR_ERROR; break;
    }

    ui_manager_->show_confirmation(
        motor_test_outcome_title(outcome),
        message,
        "OK",
        lv_color_hex(color),
        nullptr,
        nullptr);  // Single OK button
}

void MenuUIController::stop_motor_timer() {
    if (motor_timer_) {
        lv_timer_del(motor_timer_);
        motor_timer_ = nullptr;
    }
}

void MenuUIController::motor_timer_cb(lv_timer_t*) {
    if (!ui_manager_) {
        stop_motor_timer();
        BlockingOperationOverlay::getInstance().hide();
        return;
    }

    auto* hardware = ui_manager_->get_hardware_manager();
    auto* grinder = hardware ? hardware->get_grinder() : nullptr;
    const uint32_t elapsed_ms = millis() - motor_test_start_ms_;

    // is_pulse_complete() is also true when the output was stopped from elsewhere (lock-out)
    const bool completed = !grinder || grinder->is_pulse_complete();
    if (completed || elapsed_ms >= HW_MOTOR_TEST_DURATION_MS + HW_MOTOR_TEST_STOP_GRACE_MS) {
        finish_motor_test(completed, false);
        return;
    }

    char text[40];
    format_remaining(elapsed_ms, text, sizeof(text));
    BlockingOperationOverlay::getInstance().set_message(text);
}

void MenuUIController::static_motor_timer_cb(lv_timer_t* timer) {
    if (!timer) {
        return;
    }
    auto* controller = static_cast<MenuUIController*>(lv_timer_get_user_data(timer));
    if (controller) {
        controller->motor_timer_cb(timer);
    }
}
