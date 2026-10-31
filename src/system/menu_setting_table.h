#pragma once
#include <stddef.h>
#include <stdint.h>
#include "system/ui_settings.h"

// Tables of the setting toggles and sliders of the menu. Pure (no LVGL), so the screen and controller
// tables built on them are host-testable.
//
// Toggles (Bluetooth startup, logging, swipe, auto-start, auto-return): one row per setting with label,
// UiSettings getter/setter, serial log strings and an optional follow-up. The BLE "Enabled" toggle and
// the WiFi toggles have custom logic and are not part of it.

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

// ---- Sliders (brightness normal/screensaver, purge amount, freshness) ----

enum class SliderSetting : unsigned char {
    BRIGHTNESS_NORMAL,
    BRIGHTNESS_SCREENSAVER,
    PURGE_AMOUNT,
    FRESHNESS,
    COUNT
};

constexpr size_t SLIDER_SETTING_COUNT = static_cast<size_t>(SliderSetting::COUNT);

// Work the controller must do after the released value is stored
enum class SliderAfter : unsigned char {
    NONE,
    RESTORE_NORMAL_BRIGHTNESS  // the screensaver slider previews the dimmed level while dragged
};

// A position is the raw slider value (percent, 0.1 g units or freshness step index). Every hook is set
// except live_brightness (only the brightness sliders preview on the display while dragged).
struct SliderSettingDef {
    const char* row_label;
    uint32_t color;  // 24-bit RGB of the slider indicator
    int min;
    int max;
    int (*load_pos)(const UiSettings& settings);               // stored value -> slider position
    int (*clamp_pos)(int pos);                                 // position the slider is snapped back to
    const char* (*label)(char* out, size_t out_len, int pos);  // row label text of a position
    bool (*store)(UiSettings& settings, int pos);              // persist a (clamped) position
    float (*live_brightness)(int pos);                         // display brightness fraction, or nullptr
    SliderAfter after;
    const char* log_name;                                      // prefix of the serial log line
};

// Definition of one slider (id must be below SliderSetting::COUNT)
const SliderSettingDef& slider_setting_def(SliderSetting id);
