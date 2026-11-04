#pragma once
#include "system/settings_store.h"

// NVS namespaces and keys of the persisted user settings. These are the ONLY definitions of the strings:
// they are bit-compatible with what shipped firmware writes, so never rename them.
// (constexpr in the header so other code can static_assert against them with same_str.)
namespace ui_settings_keys {

constexpr bool same_str(const char* a, const char* b) {
    return *a == *b && (*a == '\0' || same_str(a + 1, b + 1));
}

constexpr const char* kBluetoothNs = "bluetooth";
constexpr const char* kBleStartup = "startup";

constexpr const char* kLoggingNs = "logging";
constexpr const char* kLoggingEnabled = "enabled";

constexpr const char* kSwipeNs = "swipe";
constexpr const char* kSwipeEnabled = "enabled";

constexpr const char* kAutoGrindNs = "autogrind";
constexpr const char* kAutoStart = "auto_start";
constexpr const char* kAutoReturn = "auto_return";

constexpr const char* kBrightnessNs = "brightness";
constexpr const char* kBrightnessNormal = "normal";
constexpr const char* kBrightnessScreensaver = "screensaver";

// Screen dim / off timeouts (whole minutes, 0 = Never)
constexpr const char* kScreenNs = "screen";
constexpr const char* kScreenDimMin = "dim_min";
constexpr const char* kScreenOffMin = "off_min";

// Shared grinder namespace (the firmware store routes it through the shared Preferences handle)
constexpr const char* kGrinderNs = "grinder";
constexpr const char* kGrindMode = "grind_mode";
constexpr const char* kPurgeMode = "grinder_mode";
constexpr const char* kPurgeAmountG = "grinder_amt_g";
constexpr const char* kFreshnessHours = "freshness_hrs";

}  // namespace ui_settings_keys

// Typed access to the persisted UI/grind settings. Stateless (no cache): every call goes to the store.
// Getters return the RAW stored value or the legacy default; clamping and conversion live in
// menu_setting_models. Setters return false when the store did not persist the value.
class UiSettings {
public:
    explicit UiSettings(SettingsStore& store) : store_(store) {}

    bool ble_startup() const;
    bool set_ble_startup(bool enabled);

    bool logging_enabled() const;
    bool set_logging_enabled(bool enabled);

    bool swipe_enabled() const;
    bool set_swipe_enabled(bool enabled);

    bool auto_start() const;
    bool set_auto_start(bool enabled);

    bool auto_return() const;
    bool set_auto_return(bool enabled);

    // Brightness as a fraction 0..1 (percent / 100.0f)
    float brightness_normal() const;
    bool set_brightness_normal(float fraction);

    float brightness_screensaver() const;
    bool set_brightness_screensaver(float fraction);

    // Raw stored minutes (0 = Never); unlisted values are normalised by the caller (system/screen_power.h)
    int screen_dim_minutes() const;
    bool set_screen_dim_minutes(int minutes);

    int screen_off_minutes() const;
    bool set_screen_off_minutes(int minutes);

    // Stored GrindMode value; read-only here (ProfileController owns writes)
    int grind_mode_raw() const;

    // 0 = Prime, 1 = Purge (GrinderPurgeMode)
    int purge_mode() const;
    bool set_purge_mode(int mode);

    float purge_amount_g() const;
    bool set_purge_amount_g(float amount_g);

    float freshness_hours() const;
    bool set_freshness_hours(float hours);

private:
    SettingsStore& store_;
};
