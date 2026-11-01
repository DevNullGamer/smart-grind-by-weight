#pragma once
#include <Preferences.h>
#include "../config/constants.h"
#include "grind_mode.h"

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

public:
    void init(Preferences* prefs);
    void load_profiles();
    // Returns false if any of the six values could not be persisted
    bool save_profiles();
    void save_current_profile();
    
    void set_current_profile(int index);
    int get_current_profile() const { return current_profile; }
    float get_current_weight() const { return profiles[current_profile].weight; }
    float get_current_time() const { return profiles[current_profile].time_seconds; }
    const char* get_current_name() const { return profiles[current_profile].name; }
    
    // Valid value and index only; returns false if it was refused or could not be persisted (the RAM value then keeps
    // its previous value)
    bool set_profile_weight(int index, float weight);
    float get_profile_weight(int index) const;
    const char* get_profile_name(int index) const;
    bool set_profile_time(int index, float seconds);
    float get_profile_time(int index) const;
    
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
