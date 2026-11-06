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
    GLINK_LINK,  // Bluetooth page "GaggiMate link"; touch-only, not a web panel setting (the portal fields end at AUTO_RETURN)
    COUNT
};

constexpr size_t TOGGLE_SETTING_COUNT = static_cast<size_t>(ToggleSetting::COUNT);

// Work the controller must do after the new value is stored
enum class ToggleAfter : unsigned char {
    NONE,
    REFRESH_AUTO_ACTIONS, // UIManager::refresh_auto_action_settings() (motor-adjacent: re-reads auto-start/return)
    APPLY_GLINK_LINK      // BluetoothManager::set_glink_link() (starts or stops the persistent GaggiMate link)
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

// ---- Sliders (brightness normal/screensaver, purge amount, freshness, screen dim/off timeouts) ----

enum class SliderSetting : unsigned char {
    BRIGHTNESS_NORMAL,
    BRIGHTNESS_SCREENSAVER,
    PURGE_AMOUNT,
    FRESHNESS,
    SCREEN_DIM_TIMEOUT,
    SCREEN_OFF_TIMEOUT,
    COUNT
};

constexpr size_t SLIDER_SETTING_COUNT = static_cast<size_t>(SliderSetting::COUNT);

// Work the controller must do after the released value is stored
enum class SliderAfter : unsigned char {
    NONE,
    RESTORE_NORMAL_BRIGHTNESS,  // the screensaver slider previews the dimmed level while dragged
    RELOAD_SCREEN_TIMEOUTS      // UIManager::refresh_screen_timeout_settings() (the controller caches both timeouts)
};

// A position is the raw slider value (percent, 0.1 g units or a step index). Every hook is set
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

// ---- Radio groups (grind mode, purge mode, hardware board override) ----

enum class RadioSetting : unsigned char {
    GRIND_MODE,      // Weight / Time (motor-adjacent: changes the grind mode of the profile controller)
    PURGE_MODE,      // Keep / Remove (motor-adjacent: decides what happens to the primed grinds)
    BOARD_OVERRIDE,  // Auto / V1 / V2
    COUNT
};

constexpr size_t RADIO_SETTING_COUNT = static_cast<size_t>(RadioSetting::COUNT);
constexpr size_t RADIO_MAX_OPTIONS = 3;

// Layout of one radio group. The button index is the stored value of purge mode and board override;
// grind mode maps through grind_mode_radio_index() (menu_setting_models.h).
struct RadioSettingDef {
    const char* labels[RADIO_MAX_OPTIONS];
    int count;
    int initial_index;  // selection before show() loads the stored value
    int width;          // button width in px, -1 = fill the row evenly
    int height;         // button height in px
};

// Definition of one radio group (id must be below RadioSetting::COUNT)
const RadioSettingDef& radio_setting_def(RadioSetting id);
