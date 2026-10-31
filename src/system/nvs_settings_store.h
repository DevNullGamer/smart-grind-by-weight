#pragma once
#include <Preferences.h>
#include "system/settings_store.h"
#include "system/ui_settings.h"

// SettingsStore backed by Arduino Preferences, behaving exactly like the legacy call sites:
// - namespace "grinder" goes through the SHARED handle (opened by HardwareManager::init and used by
//   GrindController/ProfileController). Until set_grinder_handle() is called, gets return the default and
//   puts are dropped (same as the legacy `if (prefs)` guards).
// - every other namespace uses a short-lived local Preferences per call (read-only for gets, writable for puts).
class NvsSettingsStore : public SettingsStore {
public:
    // Attach the shared "grinder" handle (call after hardware_manager.init())
    void set_grinder_handle(Preferences* prefs) { grinder_prefs_ = prefs; }

    bool get_bool(const char* ns, const char* key, bool default_value) override;
    bool put_bool(const char* ns, const char* key, bool value) override;

    int32_t get_int(const char* ns, const char* key, int32_t default_value) override;
    bool put_int(const char* ns, const char* key, int32_t value) override;

    float get_float(const char* ns, const char* key, float default_value) override;
    bool put_float(const char* ns, const char* key, float value) override;

private:
    bool is_grinder_ns(const char* ns) const { return ui_settings_keys::same_str(ns, ui_settings_keys::kGrinderNs); }

    Preferences* grinder_prefs_ = nullptr;
};

extern NvsSettingsStore nvs_settings_store;
extern UiSettings ui_settings;
