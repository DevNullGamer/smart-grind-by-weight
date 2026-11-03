#include "portal_insights.h"
#include "../logging/grind_history.h"

void portal_history_from_snapshot(const GrindHistorySnapshot& snapshot, PortalHistory* out) {
    if (!out) return;
    *out = PortalHistory();
    out->sequence = snapshot.sequence;
    out->loaded = snapshot.loaded;
    out->persistent = snapshot.persistent;
    size_t n = snapshot.count;
    if (n > GRIND_HISTORY_RING_CAPACITY) n = GRIND_HISTORY_RING_CAPACITY;
    if (n > PORTAL_HISTORY_MAX) n = PORTAL_HISTORY_MAX;
    for (size_t i = 0; i < n; i++) {
        const GrindHistoryEntry& e = snapshot.entries[i];
        PortalHistoryEntry& p = out->entries[i];
        p.id = e.session_id;
        p.ended_s = e.ended_s;
        p.time_ms = e.time_ms;
        p.motor_ms = e.motor_ms;
        p.target = e.target;
        p.final_g = e.final_g;
        p.profile = e.profile;
        p.mode = e.mode;
        p.pulses = e.pulses;
        p.result = e.result;
        p.earlier_boot = e.earlier_boot;
    }
    out->count = static_cast<uint8_t>(n);
}
