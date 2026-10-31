#include "system/ui_settings.h"
#include "config/grind_control.h"
#include "config/user.h"
#include "controllers/grind_mode.h"

using namespace ui_settings_keys;

bool UiSettings::ble_startup() const { return store_.get_bool(kBluetoothNs, kBleStartup, true); }
bool UiSettings::set_ble_startup(bool enabled) { return store_.put_bool(kBluetoothNs, kBleStartup, enabled); }

bool UiSettings::logging_enabled() const { return store_.get_bool(kLoggingNs, kLoggingEnabled, false); }
bool UiSettings::set_logging_enabled(bool enabled) { return store_.put_bool(kLoggingNs, kLoggingEnabled, enabled); }

bool UiSettings::swipe_enabled() const { return store_.get_bool(kSwipeNs, kSwipeEnabled, false); }
bool UiSettings::set_swipe_enabled(bool enabled) { return store_.put_bool(kSwipeNs, kSwipeEnabled, enabled); }

bool UiSettings::auto_start() const { return store_.get_bool(kAutoGrindNs, kAutoStart, false); }
bool UiSettings::set_auto_start(bool enabled) { return store_.put_bool(kAutoGrindNs, kAutoStart, enabled); }

bool UiSettings::auto_return() const { return store_.get_bool(kAutoGrindNs, kAutoReturn, false); }
bool UiSettings::set_auto_return(bool enabled) { return store_.put_bool(kAutoGrindNs, kAutoReturn, enabled); }

float UiSettings::brightness_normal() const {
    return store_.get_float(kBrightnessNs, kBrightnessNormal, USER_SCREEN_BRIGHTNESS_NORMAL);
}
bool UiSettings::set_brightness_normal(float fraction) {
    return store_.put_float(kBrightnessNs, kBrightnessNormal, fraction);
}

float UiSettings::brightness_screensaver() const {
    return store_.get_float(kBrightnessNs, kBrightnessScreensaver, USER_SCREEN_BRIGHTNESS_DIMMED);
}
bool UiSettings::set_brightness_screensaver(float fraction) {
    return store_.put_float(kBrightnessNs, kBrightnessScreensaver, fraction);
}

int UiSettings::grind_mode_raw() const {
    return store_.get_int(kGrinderNs, kGrindMode, static_cast<int>(GrindMode::WEIGHT));
}

int UiSettings::purge_mode() const { return store_.get_int(kGrinderNs, kPurgeMode, GRIND_PURGE_MODE_DEFAULT); }
bool UiSettings::set_purge_mode(int mode) { return store_.put_int(kGrinderNs, kPurgeMode, mode); }

float UiSettings::purge_amount_g() const {
    return store_.get_float(kGrinderNs, kPurgeAmountG, GRIND_PURGE_AMOUNT_DEFAULT_G);
}
bool UiSettings::set_purge_amount_g(float amount_g) { return store_.put_float(kGrinderNs, kPurgeAmountG, amount_g); }

float UiSettings::freshness_hours() const {
    return store_.get_float(kGrinderNs, kFreshnessHours, GRIND_FRESHNESS_DEFAULT_HOURS);
}
bool UiSettings::set_freshness_hours(float hours) { return store_.put_float(kGrinderNs, kFreshnessHours, hours); }
