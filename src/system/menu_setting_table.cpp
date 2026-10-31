#include "system/menu_setting_table.h"
#include "config/theme.h"
#include "system/menu_setting_models.h"

namespace {

// Indexed by ToggleSetting; the order is the enum order
const ToggleSettingDef kToggleDefs[TOGGLE_SETTING_COUNT] = {
    {"Startup", &UiSettings::ble_startup, &UiSettings::set_ble_startup,
     "Bluetooth startup enabled", "Bluetooth startup disabled", ToggleAfter::NONE},
    {"Logging", &UiSettings::logging_enabled, &UiSettings::set_logging_enabled,
     "Logging enabled", "Logging disabled", ToggleAfter::NONE},
    {"Swipe", &UiSettings::swipe_enabled, &UiSettings::set_swipe_enabled,
     "Grind mode swipe gestures enabled", "Grind mode swipe gestures disabled", ToggleAfter::NONE},
    {"Start", &UiSettings::auto_start, &UiSettings::set_auto_start,
     "Auto-start on cup enabled", "Auto-start on cup disabled", ToggleAfter::REFRESH_AUTO_ACTIONS},
    {"Return", &UiSettings::auto_return, &UiSettings::set_auto_return,
     "Auto return on cup removal enabled", "Auto return on cup removal disabled", ToggleAfter::REFRESH_AUTO_ACTIONS},
};

// ---- Slider hooks ----

int clamp_brightness_pos(int pos) { return brightness_clamp_percent(pos); }
int clamp_purge_pos(int pos) {
    if (pos < purge_slider_min()) return purge_slider_min();
    if (pos > purge_slider_max()) return purge_slider_max();
    return pos;
}
int clamp_freshness_pos(int pos) {
    const int max = kFreshnessStepCount - 1;
    return pos < 0 ? 0 : (pos > max ? max : pos);
}

int load_brightness_normal(const UiSettings& s) { return brightness_percent_from_fraction(s.brightness_normal()); }
int load_brightness_screensaver(const UiSettings& s) {
    return brightness_percent_from_fraction(s.brightness_screensaver());
}
int load_purge(const UiSettings& s) { return purge_slider_from_amount(s.purge_amount_g()); }
int load_freshness(const UiSettings& s) { return freshness_index_from_hours(s.freshness_hours()); }

const char* label_brightness_normal(char* out, size_t len, int pos) { return brightness_label(out, len, pos, false); }
const char* label_brightness_screensaver(char* out, size_t len, int pos) { return brightness_label(out, len, pos, true); }
const char* label_purge(char* out, size_t len, int pos) { return purge_label(out, len, purge_amount_from_slider(pos)); }
const char* label_freshness(char* out, size_t len, int pos) {
    return freshness_label(out, len, freshness_hours_from_index(pos));
}

bool store_brightness_normal(UiSettings& s, int pos) {
    return s.set_brightness_normal(brightness_fraction_from_percent(pos));
}
bool store_brightness_screensaver(UiSettings& s, int pos) {
    return s.set_brightness_screensaver(brightness_fraction_from_percent(pos));
}
bool store_purge(UiSettings& s, int pos) { return s.set_purge_amount_g(purge_amount_from_slider(pos)); }
bool store_freshness(UiSettings& s, int pos) { return s.set_freshness_hours(freshness_hours_from_index(pos)); }

float live_brightness(int pos) { return brightness_fraction_from_percent(pos); }

// Indexed by SliderSetting; the order is the enum order
const SliderSettingDef kSliderDefs[SLIDER_SETTING_COUNT] = {
    {"Brightness", THEME_COLOR_ACCENT, 0, 100, load_brightness_normal, clamp_brightness_pos,
     label_brightness_normal, store_brightness_normal, live_brightness, SliderAfter::NONE,
     "Normal brightness set to"},
    {"Screensaver", THEME_COLOR_WARNING, 0, 100, load_brightness_screensaver, clamp_brightness_pos,
     label_brightness_screensaver, store_brightness_screensaver, live_brightness,
     SliderAfter::RESTORE_NORMAL_BRIGHTNESS, "Screensaver brightness set to"},
    {"Amount", THEME_COLOR_ACCENT, purge_slider_min(), purge_slider_max(), load_purge, clamp_purge_pos,
     label_purge, store_purge, nullptr, SliderAfter::NONE, "Grinder purge amount set to"},
    {"Freshness", THEME_COLOR_ACCENT, 0, kFreshnessStepCount - 1, load_freshness, clamp_freshness_pos,
     label_freshness, store_freshness, nullptr, SliderAfter::NONE, "Grind freshness hours set to"},
};

// Indexed by RadioSetting; the order is the enum order
const RadioSettingDef kRadioDefs[RADIO_SETTING_COUNT] = {
    {{"Weight", "Time", nullptr}, 2, 0, 135, 100},
    {{"Keep", "Remove", nullptr}, 2, 1, 135, 100},
    {{"Auto", "V1", "V2"}, 3, 0, -1, 100},
};

}  // namespace

const ToggleSettingDef& toggle_setting_def(ToggleSetting id) {
    return kToggleDefs[static_cast<size_t>(id)];
}

const SliderSettingDef& slider_setting_def(SliderSetting id) {
    return kSliderDefs[static_cast<size_t>(id)];
}

const RadioSettingDef& radio_setting_def(RadioSetting id) {
    return kRadioDefs[static_cast<size_t>(id)];
}
