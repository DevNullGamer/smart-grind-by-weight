#include "grind_history.h"
#include <cmath>
#include <string.h>

GrindHistoryResult grind_history_result_from_string(const char* result) {
    if (!result) return GrindHistoryResult::UNKNOWN;
    if (strcmp(result, "COMPLETE") == 0) return GrindHistoryResult::COMPLETE;
    if (strcmp(result, "COMPLETE - MAX PULSES") == 0) return GrindHistoryResult::MAX_PULSES;
    if (strcmp(result, "OVERSHOOT") == 0) return GrindHistoryResult::OVERSHOOT;
    if (strcmp(result, "TIMEOUT") == 0) return GrindHistoryResult::TIMEOUT;
    return GrindHistoryResult::UNKNOWN;
}

bool grind_history_result_valid(uint8_t code) {
    return code <= static_cast<uint8_t>(GrindHistoryResult::TIMEOUT) ||
           code == static_cast<uint8_t>(GrindHistoryResult::UNKNOWN);
}

bool grind_history_entry_valid(const GrindHistoryEntry& entry) {
    return std::isfinite(entry.target) && std::isfinite(entry.final_g) && entry.profile <= GRIND_HISTORY_PROFILE_MAX &&
           entry.mode <= GRIND_HISTORY_MODE_MAX && grind_history_result_valid(entry.result);
}

void grind_history_entry_sanitize(GrindHistoryEntry* entry) {
    if (!entry) return;
    if (!std::isfinite(entry->target)) entry->target = 0.0f;
    if (!std::isfinite(entry->final_g)) entry->final_g = 0.0f;
    if (entry->profile > GRIND_HISTORY_PROFILE_MAX) entry->profile = GRIND_HISTORY_PROFILE_MAX;
    if (entry->mode > GRIND_HISTORY_MODE_MAX) entry->mode = GRIND_HISTORY_MODE_MAX;
    if (!grind_history_result_valid(entry->result)) entry->result = static_cast<uint8_t>(GrindHistoryResult::UNKNOWN);
}

void GrindHistoryRing::clear() {
    next_ = 0;
    count_ = 0;
}

void GrindHistoryRing::push(const GrindHistoryEntry& entry) {
    items_[next_] = entry;
    next_ = static_cast<uint8_t>((next_ + 1) % GRIND_HISTORY_RING_CAPACITY);
    if (count_ < GRIND_HISTORY_RING_CAPACITY) count_++;
    sequence_++;
}

uint8_t GrindHistoryRing::copy_newest_first(GrindHistoryEntry* out, size_t max) const {
    if (!out) return 0;
    const size_t n = count_ < max ? count_ : max;
    for (size_t i = 0; i < n; i++) {
        // i = 0 is the slot before next_
        const size_t slot = (static_cast<size_t>(next_) + GRIND_HISTORY_RING_CAPACITY - 1 - i) % GRIND_HISTORY_RING_CAPACITY;
        out[i] = items_[slot];
    }
    return static_cast<uint8_t>(n);
}

void GrindHistoryRing::mark_earlier_boot() {
    for (size_t i = 0; i < GRIND_HISTORY_RING_CAPACITY; i++) items_[i].earlier_boot = true;
}
