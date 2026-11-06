#include "profile_controller.h"
#include <Arduino.h>
#include <string.h>
#include <Preferences.h>
#include <math.h>
#include "../system/nvs_settings_store.h"

void ProfileController::init(Preferences* prefs) {
    preferences = prefs;

    // Initialize default profiles
    strcpy(profiles[0].name, "SINGLE");
    profiles[0].weight = USER_SINGLE_ESPRESSO_WEIGHT_G;
    profiles[0].time_seconds = USER_SINGLE_ESPRESSO_TIME_S;

    strcpy(profiles[1].name, "DOUBLE");
    profiles[1].weight = USER_DOUBLE_ESPRESSO_WEIGHT_G;
    profiles[1].time_seconds = USER_DOUBLE_ESPRESSO_TIME_S;

    strcpy(profiles[2].name, "CUSTOM");
    profiles[2].weight = USER_CUSTOM_PROFILE_WEIGHT_G;
    profiles[2].time_seconds = USER_CUSTOM_PROFILE_TIME_S;

    strcpy(profiles[3].name, "PROFILE");
    profiles[3].weight = USER_DOSE_PROFILE_WEIGHT_G;
    profiles[3].time_seconds = USER_DOSE_PROFILE_TIME_S;

    // Initialize default grind mode
    current_grind_mode = GrindMode::WEIGHT;

    load_profiles();
}

void ProfileController::load_profiles() {
    current_profile = preferences->getInt(ui_settings_keys::kProfileCurrent, 1);

    profiles[0].weight = preferences->getFloat(ui_settings_keys::kProfileWeight[0], USER_SINGLE_ESPRESSO_WEIGHT_G);
    profiles[1].weight = preferences->getFloat(ui_settings_keys::kProfileWeight[1], USER_DOUBLE_ESPRESSO_WEIGHT_G);
    profiles[2].weight = preferences->getFloat(ui_settings_keys::kProfileWeight[2], USER_CUSTOM_PROFILE_WEIGHT_G);

    profiles[0].time_seconds = preferences->getFloat(ui_settings_keys::kProfileTime[0], USER_SINGLE_ESPRESSO_TIME_S);
    profiles[1].time_seconds = preferences->getFloat(ui_settings_keys::kProfileTime[1], USER_DOUBLE_ESPRESSO_TIME_S);
    profiles[2].time_seconds = preferences->getFloat(ui_settings_keys::kProfileTime[2], USER_CUSTOM_PROFILE_TIME_S);

    // Profile Dose: exists exactly when a valid weight is stored (a missing key reads as NaN, which is not valid)
    const float dose_weight = preferences->getFloat(ui_settings_keys::kProfileWeight[USER_DOSE_PROFILE_INDEX], NAN);
    dose_present_ = is_weight_valid(dose_weight);
    profiles[USER_DOSE_PROFILE_INDEX].weight = dose_present_ ? dose_weight : USER_DOSE_PROFILE_WEIGHT_G;
    profiles[USER_DOSE_PROFILE_INDEX].time_seconds =
        dose_present_ ? preferences->getFloat(ui_settings_keys::kProfileTime[USER_DOSE_PROFILE_INDEX], USER_DOSE_PROFILE_TIME_S)
                      : USER_DOSE_PROFILE_TIME_S;

    // Grind sizes live in the same namespace, through the shared handle (attached before the profiles are initialised)
    grind_sizes_.attach(&nvs_settings_store);
    grind_sizes_.load();

    // Load grind mode (default to WEIGHT if not set)
    int stored_mode = preferences->getInt("grind_mode", static_cast<int>(GrindMode::WEIGHT));
    current_grind_mode = static_cast<GrindMode>(stored_mode);

    if (!profile_exists(current_profile)) {
        current_profile = 1;
    }
}

bool ProfileController::put_float_if_changed(const char* key, float value) {
    if (preferences->getFloat(key, NAN) == value) return true;  // a missing key reads as NaN, which equals nothing
    return preferences->putFloat(key, value) == sizeof(float);
}

bool ProfileController::save_profiles() {
    // Every value is attempted even after a failure (as before); the result tells the caller whether all stuck. A value
    // that is already stored is not written again (compare first, so a swipe or an unchanged save costs no flash write).
    bool stored = true;
    for (int i = 0; i < USER_PROFILE_COUNT; i++) {
        if (!profile_exists(i)) continue;  // the Profile Dose is written only once it exists (store_profile_weight)
        stored &= put_float_if_changed(ui_settings_keys::kProfileWeight[i], profiles[i].weight);
    }
    for (int i = 0; i < USER_PROFILE_COUNT; i++) {
        if (!profile_exists(i)) continue;
        stored &= put_float_if_changed(ui_settings_keys::kProfileTime[i], profiles[i].time_seconds);
    }
    return stored;
}

void ProfileController::save_current_profile() {
    if (preferences->getInt(ui_settings_keys::kProfileCurrent, -1) != current_profile) {
        preferences->putInt(ui_settings_keys::kProfileCurrent, current_profile);
    }
    save_profiles();
}

bool ProfileController::set_current_profile(int index) {
    if (!profile_exists(index)) return false;
    if (index == current_profile) return true;  // compare first: nothing to store
    current_profile = index;
    save_current_profile();
    return true;
}

bool ProfileController::set_profile_weight(int index, float weight) {
    if (profile_exists(index) && is_weight_valid(weight)) {
        const float previous = profiles[index].weight;
        profiles[index].weight = weight;
        if (save_profiles()) return true;
        profiles[index].weight = previous;  // not stored: RAM keeps matching what the grinder has saved
    }
    return false;
}

float ProfileController::get_profile_weight(int index) const {
    if (profile_exists(index)) {
        return profiles[index].weight;
    }
    return 0.0f;
}

const char* ProfileController::get_profile_name(int index) const {
    if (index >= 0 && index < USER_PROFILE_COUNT) {
        return profiles[index].name;
    }
    return "UNKNOWN";
}

bool ProfileController::set_profile_time(int index, float seconds) {
    if (profile_exists(index) && is_time_valid(seconds)) {
        const float previous = profiles[index].time_seconds;
        profiles[index].time_seconds = seconds;
        if (save_profiles()) return true;
        profiles[index].time_seconds = previous;  // not stored: RAM keeps matching what the grinder has saved
    }
    return false;
}

float ProfileController::get_profile_time(int index) const {
    if (profile_exists(index)) {
        return profiles[index].time_seconds;
    }
    return 0.0f;
}

bool ProfileController::store_profile_weight(int index, float weight) {
    if (index < 0 || index >= USER_PROFILE_COUNT || !is_weight_valid(weight)) return false;
    if (profile_exists(index)) return set_profile_weight(index, weight);

    // A size left over from an earlier dose must not reappear with the new one
    if (!grind_sizes_.clear(USER_DOSE_PROFILE_INDEX)) return false;

    // First dose: the time goes first and the weight (the marker that the profile exists) last, so a failure part-way
    // leaves a profile that still does not exist
    if (put_float_if_changed(ui_settings_keys::kProfileTime[USER_DOSE_PROFILE_INDEX], USER_DOSE_PROFILE_TIME_S) &&
        put_float_if_changed(ui_settings_keys::kProfileWeight[USER_DOSE_PROFILE_INDEX], weight)) {
        profiles[USER_DOSE_PROFILE_INDEX].weight = weight;
        profiles[USER_DOSE_PROFILE_INDEX].time_seconds = USER_DOSE_PROFILE_TIME_S;
        dose_present_ = true;
        return true;
    }
    return false;
}

bool ProfileController::remove_dose_profile() {
    // The weight is the marker: once it is gone the profile is gone, whatever fails afterwards
    const char* weight_key = ui_settings_keys::kProfileWeight[USER_DOSE_PROFILE_INDEX];
    const char* time_key = ui_settings_keys::kProfileTime[USER_DOSE_PROFILE_INDEX];
    grind_sizes_.clear(USER_DOSE_PROFILE_INDEX);  // first, so a failed marker removal below cannot leave it behind unnoticed
    if (preferences->isKey(weight_key) && !preferences->remove(weight_key)) return false;
    dose_present_ = false;
    profiles[USER_DOSE_PROFILE_INDEX].weight = USER_DOSE_PROFILE_WEIGHT_G;
    profiles[USER_DOSE_PROFILE_INDEX].time_seconds = USER_DOSE_PROFILE_TIME_S;

    if (preferences->isKey(time_key)) preferences->remove(time_key);

    if (current_profile == USER_DOSE_PROFILE_INDEX) {
        current_profile = 1;
        preferences->putInt(ui_settings_keys::kProfileCurrent, current_profile);
    }
    return true;
}

bool ProfileController::store_grind_size(int index, const GrindSize& size) {
    if (!profile_exists(index)) return false;
    return grind_sizes_.store(index, size) != GrindSizeStoreResult::FAILED;
}

bool ProfileController::is_weight_valid(float weight) const {
    return weight >= USER_MIN_TARGET_WEIGHT_G && weight <= USER_MAX_TARGET_WEIGHT_G;
}

float ProfileController::clamp_weight(float weight) const {
    if (weight < USER_MIN_TARGET_WEIGHT_G) return USER_MIN_TARGET_WEIGHT_G;
    if (weight > USER_MAX_TARGET_WEIGHT_G) return USER_MAX_TARGET_WEIGHT_G;
    return weight;
}

void ProfileController::update_current_weight(float weight) {
    if (is_weight_valid(weight)) {
        profiles[current_profile].weight = weight;
    }
}

void ProfileController::update_current_time(float seconds) {
    if (is_time_valid(seconds)) {
        profiles[current_profile].time_seconds = seconds;
    }
}

bool ProfileController::is_time_valid(float seconds) const {
    return seconds >= USER_MIN_TARGET_TIME_S && seconds <= USER_MAX_TARGET_TIME_S;
}

float ProfileController::clamp_time(float seconds) const {
    if (seconds < USER_MIN_TARGET_TIME_S) return USER_MIN_TARGET_TIME_S;
    if (seconds > USER_MAX_TARGET_TIME_S) return USER_MAX_TARGET_TIME_S;
    return seconds;
}

bool ProfileController::set_grind_mode(GrindMode mode) {
    const GrindMode previous = current_grind_mode;
    current_grind_mode = mode;
    if (save_grind_mode()) return true;
    current_grind_mode = previous;  // not stored: RAM keeps matching what the grinder has saved
    return false;
}

bool ProfileController::save_grind_mode() {
    return preferences->putInt("grind_mode", static_cast<int>(current_grind_mode)) == sizeof(int32_t);
}
