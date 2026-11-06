#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/bluetooth.h"
#include "../config/user.h"
#include "../system/settings_store.h"

// Grind size of a profile (chg020): an informational number with an optional unit that GaggiMate sends and the
// grinder only displays ("Grind: 14.5 um"). Pure and host-tested: no Arduino/ESP dependencies, no heap. The wire format
// (tenths, unit charset) is validated by bluetooth/glink_messages; this file stores, compares and formats what passed.

constexpr int32_t GRIND_SIZE_MAX_TENTHS = 9999;  // 999.9, GLINK_GRIND_SIZE_MAX in tenths
constexpr size_t GRIND_UNIT_BUF = GLINK_GRIND_UNIT_MAX_LEN + 1;  // up to 6 characters plus the terminator
constexpr size_t GRIND_SIZE_BLOB_BYTES = 12;                     // float value + 8 bytes unit (7 characters + NUL)
constexpr size_t GRIND_SIZE_TEXT_BUF = 40;                       // "Grind: 999.9 123456" is 19 characters; room to spare
constexpr int GRIND_SIZE_SLOT_COUNT = USER_PROFILE_COUNT;

struct GrindSize {
    bool present = false;
    int32_t tenths = 0;                 // 0 .. GRIND_SIZE_MAX_TENTHS, only meaningful when present
    char unit[GRIND_UNIT_BUF] = {};     // "" = no unit; only set with a size
};

// True for a size the store may hold: absent (nothing else set), or a value in range with a unit of the allowed charset
// (A-Za-z0-9 ./%-_, no leading or trailing space, terminated inside the buffer)
bool grind_size_valid(const GrindSize& size);
// Same value and unit (absent equals absent whatever the other fields hold)
bool grind_size_equal(const GrindSize& a, const GrindSize& b);

// "Grind: 14.5 um" ("Grind: 14.5" without a unit); an absent or invalid size gives "". Always terminated; returns the
// text length. cap should be GRIND_SIZE_TEXT_BUF.
size_t grind_size_format(char* out, size_t cap, const GrindSize& size);

// The stored form: 4 bytes IEEE float (little endian, tenths / 10), then the unit zero-padded to 8 bytes
size_t grind_size_encode(const GrindSize& size, uint8_t out[GRIND_SIZE_BLOB_BYTES]);
// false (and *out = none) for a wrong length, a non-finite or out-of-range float, or a unit that is not the allowed
// charset / not terminated; a size that cannot be read back is "no size"
bool grind_size_decode(const uint8_t* data, size_t len, GrindSize* out);

enum class GrindSizeStoreResult : uint8_t { UNCHANGED, STORED, FAILED };

// One slot per profile (key ui_settings_keys::kGrindSize[slot] in the grinder namespace). RAM mirrors flash: every
// write compares first, so storing what is already stored touches nothing.
class GrindSizeStore {
public:
    void attach(SettingsStore* store) { store_ = store; }
    // Reads every slot; a missing, wrong-length or corrupt entry becomes "no size"
    void load();
    // Never null; "none" for a slot outside 0..GRIND_SIZE_SLOT_COUNT-1
    const GrindSize& get(int slot) const;
    // Compare-first. Storing an absent size clears the entry. An invalid size, a bad slot, no store or a failed write
    // is FAILED and leaves RAM as it was.
    GrindSizeStoreResult store(int slot, const GrindSize& size);
    // Removes the entry; true when it is gone (also when there was none)
    bool clear(int slot);

private:
    SettingsStore* store_ = nullptr;
    GrindSize slots_[GRIND_SIZE_SLOT_COUNT];
};
