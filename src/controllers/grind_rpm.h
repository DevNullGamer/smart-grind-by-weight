#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/bluetooth.h"
#include "../config/user.h"
#include "../system/settings_store.h"
#include "grind_size.h"

// Grinder rpm of a profile (chg026): a free display label that GaggiMate sends and the grinder only shows ("600 rpm"),
// stored like the grind size. Pure and host-tested: no Arduino/ESP dependencies, no heap. The wire format is validated
// by bluetooth/glink_messages; this file stores, compares and formats what passed.

constexpr int32_t GRIND_RPM_MAX = 9999;  // GLINK_RPM_MAX
constexpr int GRIND_RPM_SLOT_COUNT = USER_PROFILE_COUNT;

struct GrindRpm {
    bool present = false;
    int32_t value = 0;  // 0 .. GRIND_RPM_MAX, only meaningful when present
};

// True for an absent rpm (nothing else set) or a value in 0..GRIND_RPM_MAX
bool grind_rpm_valid(const GrindRpm& rpm);
// Same value (absent equals absent whatever the other field holds)
bool grind_rpm_equal(const GrindRpm& a, const GrindRpm& b);

// The text of the profile's grind info: "Grind: 14.5 um" then a newline and "600 rpm"; only the line that is set;
// "" when neither is. Always terminated; returns the text length. cap should be GRIND_SIZE_TEXT_BUF.
size_t grind_info_format(char* out, size_t cap, const GrindSize& size, const GrindRpm& rpm);

enum class GrindRpmStoreResult : uint8_t { UNCHANGED, STORED, FAILED };

// One slot per profile (NVS int, key ui_settings_keys::kGrindRpm[slot] in the grinder namespace). RAM mirrors flash:
// every write compares first, so storing what is already stored touches nothing.
class GrindRpmStore {
public:
    void attach(SettingsStore* store) { store_ = store; }
    // Reads every slot; a missing or out-of-range entry becomes "no rpm"
    void load();
    // Never null; "none" for a slot outside 0..GRIND_RPM_SLOT_COUNT-1
    const GrindRpm& get(int slot) const;
    // Compare-first. Storing an absent rpm clears the entry. An invalid rpm, a bad slot, no store or a failed write is
    // FAILED and leaves RAM as it was.
    GrindRpmStoreResult store(int slot, const GrindRpm& rpm);
    // Removes the entry; true when it is gone (also when there was none)
    bool clear(int slot);

private:
    SettingsStore* store_ = nullptr;
    GrindRpm slots_[GRIND_RPM_SLOT_COUNT];
};
