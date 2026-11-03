#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"

// The recent-grinds history as the web panel sees it (chg016). Pure and host-tested (no Arduino/ESP/filesystem). The UI
// task converts the firmware's history snapshot into this type and publishes it through PortalCommandGate; the HTTP
// task only copies it. The JSON encoder for GET /api/history is portal_encode_history() in portal_api.*.

struct GrindHistorySnapshot;  // logging/grind_history.h

struct PortalHistoryEntry {
    uint32_t id = 0;           // the grind's id (not an ordering key)
    uint32_t ended_s = 0;      // uptime second of the boot the grind ended in (meaningless when earlier_boot)
    uint32_t time_ms = 0;
    uint32_t motor_ms = 0;
    float target = 0.0f;       // grams in weight mode, seconds in time mode
    float final_g = 0.0f;
    uint8_t profile = 0;
    uint8_t mode = 0;          // 0 weight, 1 time
    uint8_t pulses = 0;
    uint8_t result = 255;      // GrindHistoryResult value
    bool earlier_boot = false; // recorded before the last restart: its age is unknown
};

struct PortalHistory {
    uint32_t sequence = 0;     // changes whenever the list changed
    bool loaded = false;       // false until the boot load has finished: the list is then not real
    bool persistent = false;   // the entries are being saved to flash
    uint8_t count = 0;
    PortalHistoryEntry entries[PORTAL_HISTORY_MAX];  // newest first
};

// Field-for-field copy, at most PORTAL_HISTORY_MAX entries
void portal_history_from_snapshot(const GrindHistorySnapshot& snapshot, PortalHistory* out);

// The API name of a GrindHistoryResult value ("complete", "max_pulses", "overshoot", "timeout"); every other value,
// also 255, is "unknown". The names are part of the contract (docs/PORTAL-API.md section 7, "Recent grinds").
const char* portal_history_result_name(uint8_t result);
