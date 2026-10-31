#include "system/menu_setting_table.h"

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

}  // namespace

const ToggleSettingDef& toggle_setting_def(ToggleSetting id) {
    return kToggleDefs[static_cast<size_t>(id)];
}
