#pragma once
#include <Preferences.h>
#include "../config/constants.h"
#include "grind_mode.h"
#include "grind_size.h"

struct Profile {
    char name[USER_PROFILE_NAME_MAX_LENGTH];
    float weight;
    float time_seconds;
};

class ProfileController {
private:
    Profile profiles[USER_PROFILE_COUNT];
    int current_profile;
    GrindMode current_grind_mode;
    Preferences* preferences;
    // Profile Dose (index USER_DOSE_PROFILE_INDEX, chg020): exists once GaggiMate has set a dose. RAM mirror of "the
    // NVS key weight3 holds a valid weight"; every other profile always exists.
    bool dose_present_ = false;
    GrindSizeStore grind_sizes_;

    // Writes only when the stored value differs (or is missing); true when the value is stored afterwards
    bool put_float_if_changed(const char* key, float value);

public:
    void init(Preferences* prefs);
    void load_profiles();
    // Returns false if any of the values could not be persisted; values that are already stored are not written again
    bool save_profiles();
    void save_current_profile();

    // Selects a profile that exists; false (nothing changed) for an index outside the range or the Profile Dose
    // while no dose is set. Selecting the selected profile writes nothing.
    bool set_current_profile(int index);
    int get_current_profile() const { return current_profile; }
    float get_current_weight() const { return profiles[current_profile].weight; }
    float get_current_time() const { return profiles[current_profile].time_seconds; }
    const char* get_current_name() const { return profiles[current_profile].name; }
    
    // Valid value and index only; returns false if it was refused or could not be persisted (the RAM value then keeps
    // its previous value)
    bool set_profile_weight(int index, float weight);
    // 0 for a profile that does not exist (the Profile Dose before a dose was set), so nothing can start from it
    float get_profile_weight(int index) const;
    const char* get_profile_name(int index) const;
    bool set_profile_time(int index, float seconds);
    float get_profile_time(int index) const;

    // Profile Dose (chg020)
    bool has_dose_profile() const { return dose_present_; }
    bool profile_exists(int index) const {
        return index >= 0 && index < USER_PROFILE_COUNT && (index != USER_DOSE_PROFILE_INDEX || dose_present_);
    }
    // The BLE write path: like set_profile_weight, but the first store for the Profile Dose creates it (weight and the
    // default time are written together; the profile exists only if both stuck). An identical re-store writes nothing.
    bool store_profile_weight(int index, float weight);
    // Deletes the Profile Dose with its time and grind size; selects DOUBLE if it was selected. False if the marker key
    // could not be removed (the profile is then still there).
    bool remove_dose_profile();

    // Grind size per profile, display only. A size can only be stored for a profile that exists.
    const GrindSize& get_grind_size(int index) const { return grind_sizes_.get(index); }
    // Compare-first; an absent size clears it. False when it was refused or could not be persisted.
    bool store_grind_size(int index, const GrindSize& size);
    
    void update_current_weight(float weight);
    void update_current_time(float seconds);
    
    // Weight validation methods - single authority for all weight constraints
    bool is_weight_valid(float weight) const;
    float clamp_weight(float weight) const;

    bool is_time_valid(float seconds) const;
    float clamp_time(float seconds) const;
    
    // Grind mode persistence methods
    // Returns false if the mode could not be persisted (the RAM value then keeps its previous value)
    bool set_grind_mode(GrindMode mode);
    GrindMode get_grind_mode() const { return current_grind_mode; }
    bool save_grind_mode();
};
