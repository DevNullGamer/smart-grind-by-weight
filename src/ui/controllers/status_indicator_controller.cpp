#include "status_indicator_controller.h"

#include "../../config/constants.h"
#include "../../system/diagnostics_controller.h"
#include "../ui_manager.h"

StatusIndicatorController::StatusIndicatorController(UIManager* manager)
    : ui_manager_(manager) {}

void StatusIndicatorController::build() {
    if (!ui_manager_) {
        return;
    }

    if (ble_status_icon_) {
        return;
    }

    // Create BLE status icon (rightmost)
    ble_status_icon_ = lv_label_create(lv_screen_active());
    lv_label_set_text(ble_status_icon_, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(ble_status_icon_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ble_status_icon_, lv_color_hex(THEME_COLOR_ACCENT), 0);
    lv_obj_align(ble_status_icon_, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_hidden(ble_status_icon_, true);
    lv_obj_set_clickable(ble_status_icon_, false);

    // Create warning icon (left of BLE icon)
    warning_icon_ = lv_label_create(lv_screen_active());
    lv_label_set_text(warning_icon_, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(warning_icon_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(warning_icon_, lv_color_hex(THEME_COLOR_WARNING), 0);
    lv_obj_align(warning_icon_, LV_ALIGN_TOP_RIGHT, -45, 10);
    lv_obj_set_hidden(warning_icon_, true);
    lv_obj_set_clickable(warning_icon_, false);

    update_ble_status_icon();
    update_warning_icon();
}

void StatusIndicatorController::update() {
    // The icons sit on the active screen above every dialog and would overlap the
    // 36 pt title of a confirm dialog, so keep them hidden while one is shown
    if (ui_manager_ && ui_manager_->state_machine && ui_manager_->state_machine->is_state(UIState::CONFIRM)) {
        if (ble_status_icon_) lv_obj_set_hidden(ble_status_icon_, true);
        if (warning_icon_) lv_obj_set_hidden(warning_icon_, true);
        // Hidden behind the dialog: make the next regular update re-apply the real state
        ble_icon_state_ = BleIconState::UNKNOWN;
        warning_icon_state_ = WarningIconState::UNKNOWN;
        return;
    }
    update_ble_status_icon();
    update_warning_icon();
}

void StatusIndicatorController::update_ble_status_icon() {
    if (!ui_manager_ || !ble_status_icon_) {
        return;
    }

    auto* bluetooth = ui_manager_->bluetooth_manager;
    BleIconState state = BleIconState::HIDDEN;
    if (bluetooth && bluetooth->is_enabled()) {
        state = bluetooth->is_connected() ? BleIconState::SUCCESS : BleIconState::ACCENT;
    }
    if (state == ble_icon_state_) {
        return;
    }
    ble_icon_state_ = state;

    if (state == BleIconState::HIDDEN) {
        lv_obj_set_hidden(ble_status_icon_, true);
    } else {
        lv_obj_set_hidden(ble_status_icon_, false);
        lv_obj_set_style_text_color(ble_status_icon_,
                                    lv_color_hex(state == BleIconState::SUCCESS ? THEME_COLOR_SUCCESS
                                                                                : THEME_COLOR_ACCENT),
                                    0);
    }
}

void StatusIndicatorController::update_warning_icon() {
    if (!ui_manager_ || !warning_icon_) {
        return;
    }

    // Check if there are any diagnostic warnings
    WarningIconState state = WarningIconState::HIDDEN;
    if (ui_manager_->diagnostics_controller_ &&
        ui_manager_->diagnostics_controller_->get_highest_priority_warning() != DiagnosticCode::NONE) {
        state = WarningIconState::SHOWN;
    }
    if (state == warning_icon_state_) {
        return;
    }
    warning_icon_state_ = state;
    lv_obj_set_hidden(warning_icon_, state == WarningIconState::HIDDEN);
}
