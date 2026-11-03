#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"

// Recent-grinds history, RAM side (chg016, docs/DESIGN-NOTES.md "Persistent history (chg016)"). Pure and host-tested:
// no Arduino, no filesystem, no locking, no heap. The firmware wrapper (grind_history_fs.*) adds the spinlock and the
// files; the on-flash format is grind_history_store.*.
//
// The grinder has no wall clock, so an entry carries the grind's id and the uptime second it ended in (ended_s, the
// uptime of the boot it happened in), nothing else about time.

// Why a grind ended. The numeric values are stored in the history files: never renumber.
enum class GrindHistoryResult : uint8_t {
    COMPLETE = 0,
    MAX_PULSES = 1,
    OVERSHOOT = 2,
    TIMEOUT = 3,
    UNKNOWN = 255,
};

constexpr uint8_t GRIND_HISTORY_PROFILE_MAX = 2;  // Single, Double, Custom (PORTAL_PROFILE_COUNT - 1)
constexpr uint8_t GRIND_HISTORY_MODE_MAX = 1;     // GrindMode::WEIGHT = 0, TIME = 1
constexpr size_t GRIND_HISTORY_RING_CAPACITY = PORTAL_HISTORY_MAX;

// The result code of a session result string as grind_controller.cpp sends it ("COMPLETE", "COMPLETE - MAX PULSES",
// "OVERSHOOT", "TIMEOUT"); null, empty and anything else (case matters) is UNKNOWN.
GrindHistoryResult grind_history_result_from_string(const char* result);
// True for the values of GrindHistoryResult (also 255 = UNKNOWN)
bool grind_history_result_valid(uint8_t code);

struct GrindHistoryEntry {
    uint32_t session_id = 0;  // the grind's id (monotonic, wraps; NOT used for ordering: file order is)
    uint32_t ended_s = 0;     // uptime second of the boot the grind ended in
    uint32_t time_ms = 0;     // grind duration
    uint32_t motor_ms = 0;    // motor-on time
    float target = 0.0f;      // grams in weight mode, seconds in time mode
    float final_g = 0.0f;     // settled weight
    uint8_t profile = 0;      // 0 .. GRIND_HISTORY_PROFILE_MAX
    uint8_t mode = 0;         // 0 weight, 1 time
    uint8_t pulses = 0;
    uint8_t result = static_cast<uint8_t>(GrindHistoryResult::UNKNOWN);
    bool earlier_boot = false;  // RAM only: loaded from the files, so its ended_s belongs to another boot
};

// True when the entry could be stored and read back as it is: finite weights, profile and mode in range, a known result
bool grind_history_entry_valid(const GrindHistoryEntry& entry);
// Brings out-of-range values into range before an entry is stored (non-finite weights become 0, profile and mode are
// clamped, an unknown result becomes UNKNOWN). The firmware calls this once, where the entry is made.
void grind_history_entry_sanitize(GrindHistoryEntry* entry);

// What the UI task copies: a consistent view of the ring and of the store state, taken under one lock
struct GrindHistorySnapshot {
    uint32_t sequence = 0;     // grinds recorded or loaded so far (wraps); changes whenever the list changed
    bool loaded = false;       // the boot load has finished (before that the list is not real, not merely empty)
    bool persistent = false;   // the entries are being saved to flash
    uint8_t count = 0;
    GrindHistoryEntry entries[GRIND_HISTORY_RING_CAPACITY];  // newest first
};

// The newest GRIND_HISTORY_RING_CAPACITY entries. Fixed storage, no allocation; every index is checked.
class GrindHistoryRing {
public:
    GrindHistoryRing() = default;
    // first_sequence: where the push counter starts (tests use it to reach the 2^32 wrap)
    explicit GrindHistoryRing(uint32_t first_sequence) : sequence_(first_sequence) {}
    void clear();
    // Adds an entry as the newest; the oldest one drops out when the ring is full
    void push(const GrindHistoryEntry& entry);
    uint8_t count() const { return count_; }
    // Number of push() calls since the ring was made (wraps at 2^32); never reset by clear()
    uint32_t sequence() const { return sequence_; }
    // Copies up to max entries, newest first; returns how many
    uint8_t copy_newest_first(GrindHistoryEntry* out, size_t max) const;
    // Flags every entry as recorded in an earlier boot (a ring made by the boot load is not "this boot")
    void mark_earlier_boot();

private:
    GrindHistoryEntry items_[GRIND_HISTORY_RING_CAPACITY];
    uint8_t next_ = 0;   // slot the next push writes
    uint8_t count_ = 0;
    uint32_t sequence_ = 0;
};
