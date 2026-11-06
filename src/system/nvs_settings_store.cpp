#include "system/nvs_settings_store.h"
#include "controllers/grind_controller.h"
#include "config/constants.h"

// Globals (same pattern as statistics_manager). Order is irrelevant: the store holds no handle until attached
// and UiSettings only keeps a reference.
NvsSettingsStore nvs_settings_store;
UiSettings ui_settings(nvs_settings_store);

// The keys here are the only definition site for the facade; the controllers still hold their own copies
// until they migrate, so a one-sided edit must fail the build.
static_assert(ui_settings_keys::same_str(ui_settings_keys::kPurgeMode, GrindController::PREF_KEY_GRINDER_MODE),
              "purge mode NVS key differs from GrindController::PREF_KEY_GRINDER_MODE");
static_assert(ui_settings_keys::same_str(ui_settings_keys::kPurgeAmountG, GrindController::PREF_KEY_GRINDER_AMOUNT_G),
              "purge amount NVS key differs from GrindController::PREF_KEY_GRINDER_AMOUNT_G");
static_assert(ui_settings_keys::same_str(ui_settings_keys::kFreshnessHours, GrindController::PREF_KEY_GRIND_FRESHNESS_HOURS),
              "freshness NVS key differs from GrindController::PREF_KEY_GRIND_FRESHNESS_HOURS");

// Defaults the facade hard-codes (ui_settings.cpp) or takes from macros must match the settings table
static_assert(GRIND_PURGE_MODE_DEFAULT == 1, "purge mode default must stay Purge (1)");
static_assert(GRIND_PURGE_AMOUNT_DEFAULT_G == 1.0f, "purge amount default must stay 1.0 g");
static_assert(GRIND_FRESHNESS_DEFAULT_HOURS == 8.0f, "freshness default must stay 8 h");
static_assert(USER_SCREEN_BRIGHTNESS_NORMAL == 1.0f, "normal brightness default must stay 1.0");
static_assert(USER_SCREEN_BRIGHTNESS_DIMMED == 0.35f, "dimmed brightness default must stay 0.35");
static_assert(static_cast<int>(GrindMode::WEIGHT) == 0, "GrindMode::WEIGHT must stay 0");

bool NvsSettingsStore::get_bool(const char* ns, const char* key, bool default_value) {
    if (is_grinder_ns(ns)) return grinder_prefs_ ? grinder_prefs_->getBool(key, default_value) : default_value;
    Preferences prefs;
    prefs.begin(ns, true);
    bool value = prefs.getBool(key, default_value);
    prefs.end();
    return value;
}

bool NvsSettingsStore::put_bool(const char* ns, const char* key, bool value) {
    if (is_grinder_ns(ns)) return grinder_prefs_ && grinder_prefs_->putBool(key, value) > 0;
    Preferences prefs;
    prefs.begin(ns, false);
    size_t written = prefs.putBool(key, value);
    prefs.end();
    return written > 0;
}

int32_t NvsSettingsStore::get_int(const char* ns, const char* key, int32_t default_value) {
    if (is_grinder_ns(ns)) return grinder_prefs_ ? grinder_prefs_->getInt(key, default_value) : default_value;
    Preferences prefs;
    prefs.begin(ns, true);
    int32_t value = prefs.getInt(key, default_value);
    prefs.end();
    return value;
}

bool NvsSettingsStore::put_int(const char* ns, const char* key, int32_t value) {
    if (is_grinder_ns(ns)) return grinder_prefs_ && grinder_prefs_->putInt(key, value) > 0;
    Preferences prefs;
    prefs.begin(ns, false);
    size_t written = prefs.putInt(key, value);
    prefs.end();
    return written > 0;
}

float NvsSettingsStore::get_float(const char* ns, const char* key, float default_value) {
    if (is_grinder_ns(ns)) return grinder_prefs_ ? grinder_prefs_->getFloat(key, default_value) : default_value;
    Preferences prefs;
    prefs.begin(ns, true);
    float value = prefs.getFloat(key, default_value);
    prefs.end();
    return value;
}

bool NvsSettingsStore::put_float(const char* ns, const char* key, float value) {
    if (is_grinder_ns(ns)) return grinder_prefs_ && grinder_prefs_->putFloat(key, value) > 0;
    Preferences prefs;
    prefs.begin(ns, false);
    size_t written = prefs.putFloat(key, value);
    prefs.end();
    return written > 0;
}

size_t NvsSettingsStore::get_blob(const char* ns, const char* key, uint8_t* buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    if (is_grinder_ns(ns)) return grinder_prefs_ ? grinder_prefs_->getBytes(key, buf, cap) : 0;
    Preferences prefs;
    if (!prefs.begin(ns, true)) return 0;
    // getBytes returns 0 when the stored entry is longer than cap, so a blob is never silently truncated
    size_t got = prefs.getBytes(key, buf, cap);
    prefs.end();
    return got;
}

bool NvsSettingsStore::put_blob(const char* ns, const char* key, const uint8_t* data, size_t len) {
    if (len == 0) return erase_blob(ns, key);
    if (!data) return false;
    if (is_grinder_ns(ns)) return grinder_prefs_ && grinder_prefs_->putBytes(key, data, len) == len;
    Preferences prefs;
    if (!prefs.begin(ns, false)) return false;
    size_t written = prefs.putBytes(key, data, len);  // one nvs_set_blob + commit: a failure keeps the old entry
    prefs.end();
    return written == len;
}

bool NvsSettingsStore::erase_blob(const char* ns, const char* key) {
    if (is_grinder_ns(ns)) return grinder_prefs_ && (!grinder_prefs_->isKey(key) || grinder_prefs_->remove(key));
    Preferences prefs;
    if (!prefs.begin(ns, false)) return false;
    const bool gone = !prefs.isKey(key) || prefs.remove(key);
    prefs.end();
    return gone;
}
