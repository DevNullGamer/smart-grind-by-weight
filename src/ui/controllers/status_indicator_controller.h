#pragma once
#include <cstdint>
#include <lvgl.h>

class UIManager;

// Shows Bluetooth connection status icon with color coding
// Shows diagnostic warning icon when issues detected

class StatusIndicatorController {
public:
    explicit StatusIndicatorController(UIManager* manager);

    void build();
    void update();

private:
    void update_ble_status_icon();
    void update_warning_icon();

    // Last state pushed to LVGL. Style calls always invalidate the object (a full-width strip
    // flush), so LVGL is only touched when the state changes. UNKNOWN forces the next apply.
    enum class BleIconState : uint8_t { UNKNOWN, HIDDEN, ACCENT, SUCCESS };
    enum class WarningIconState : uint8_t { UNKNOWN, HIDDEN, SHOWN };

    UIManager* ui_manager_;
    BleIconState ble_icon_state_ = BleIconState::UNKNOWN;
    WarningIconState warning_icon_state_ = WarningIconState::UNKNOWN;
    lv_obj_t* ble_status_icon_ = nullptr;
    lv_obj_t* warning_icon_ = nullptr;
};
