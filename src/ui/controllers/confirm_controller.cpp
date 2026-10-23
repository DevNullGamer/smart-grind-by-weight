#include "confirm_controller.h"

#include <Arduino.h>
#include <utility>

#include "../../system/state_machine.h"
#include "../ui_manager.h"

ConfirmUIController::ConfirmUIController(UIManager* manager)
    : ui_manager_(manager), previous_state_(UIState::READY) {}

void ConfirmUIController::register_events() {
    if (!ui_manager_) {
        return;
    }

    if (auto* confirm_btn = ui_manager_->confirm_screen.get_confirm_button()) {
        lv_obj_add_event_cb(confirm_btn, [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            if (auto* controller = static_cast<ConfirmUIController*>(lv_event_get_user_data(e))) {
                controller->handle_confirm();
            }
        }, LV_EVENT_CLICKED, this);
    }

    if (auto* cancel_btn = ui_manager_->confirm_screen.get_cancel_button()) {
        lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            if (auto* controller = static_cast<ConfirmUIController*>(lv_event_get_user_data(e))) {
                controller->handle_cancel();
            }
        }, LV_EVENT_CLICKED, this);
    }
}

void ConfirmUIController::update() {}

void ConfirmUIController::show(const char* title,
                               const char* message,
                               const char* confirm_text,
                               lv_color_t confirm_color,
                               std::function<void()> on_confirm,
                               const char* cancel_text,
                               std::function<void()> on_cancel) {
    if (!ui_manager_) {
        return;
    }

    previous_state_ = ui_manager_->state_machine
                          ? ui_manager_->state_machine->get_current_state()
                          : UIState::READY;
    on_confirm_ = std::move(on_confirm);
    on_cancel_ = std::move(on_cancel);
    tap_guard_active_ = false;

    ui_manager_->confirm_screen.show(title, message, confirm_text, confirm_color, cancel_text);
    ui_manager_->switch_to_state(UIState::CONFIRM);
}

void ConfirmUIController::handle_confirm() {
    if (!ui_manager_ || taps_guarded()) {
        return;
    }

    auto callback = std::move(on_confirm_);
    on_confirm_ = nullptr;
    if (callback) {
        callback();
    }

    if (ui_manager_->state_machine && ui_manager_->state_machine->is_state(UIState::CONFIRM)) {
        ui_manager_->switch_to_state(previous_state_);
    }

    reset_callbacks();
}

void ConfirmUIController::handle_cancel() {
    if (!ui_manager_ || taps_guarded()) {
        return;
    }

    auto callback = std::move(on_cancel_);
    on_cancel_ = nullptr;
    if (callback) {
        callback();
    }

    if (ui_manager_->state_machine && ui_manager_->state_machine->is_state(UIState::CONFIRM)) {
        ui_manager_->switch_to_state(previous_state_);
    }

    reset_callbacks();
}

void ConfirmUIController::guard_taps(uint32_t duration_ms) {
    tap_guard_until_ms_ = millis() + duration_ms;
    tap_guard_active_ = true;
}

bool ConfirmUIController::taps_guarded() const {
    return tap_guard_active_ && static_cast<int32_t>(tap_guard_until_ms_ - millis()) > 0;
}

void ConfirmUIController::dismiss() {
    if (!ui_manager_ || !ui_manager_->state_machine || !ui_manager_->state_machine->is_state(UIState::CONFIRM)) {
        return;
    }
    reset_callbacks();
    ui_manager_->switch_to_state(previous_state_);
}

void ConfirmUIController::reset_callbacks() {
    on_confirm_ = nullptr;
    on_cancel_ = nullptr;
}
