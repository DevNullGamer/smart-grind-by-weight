#pragma once
#include <stddef.h>
#include "system/ui_settings.h"

// Table of the five on/off setting toggles of the menu (Bluetooth startup, logging, swipe, auto-start,
// auto-return). One row per setting: label, UiSettings getter/setter, serial log strings and an optional
// follow-up. Pure (no LVGL), so the screen and controller tables built on it are host-testable.
// The BLE "Enabled" toggle and the WiFi toggles have custom logic and are not part of it.

enum class ToggleSetting : unsigned char {
    BLE_STARTUP,
    LOGGING,
    SWIPE,
    AUTO_START,
    AUTO_RETURN,
    COUNT
};

constexpr size_t TOGGLE_SETTING_COUNT = static_cast<size_t>(ToggleSetting::COUNT);

// Work the controller must do after the new value is stored
enum class ToggleAfter : unsigned char {
    NONE,
    REFRESH_AUTO_ACTIONS  // UIManager::refresh_auto_action_settings() (motor-adjacent: re-reads auto-start/return)
};

struct ToggleSettingDef {
    const char* label;  // row label on the menu page
    bool (UiSettings::*get)() const;
    bool (UiSettings::*set)(bool enabled);
    const char* log_on;   // serial log line when switched on
    const char* log_off;  // serial log line when switched off
    ToggleAfter after;
};

// Definition of one setting (id must be below ToggleSetting::COUNT)
const ToggleSettingDef& toggle_setting_def(ToggleSetting id);
