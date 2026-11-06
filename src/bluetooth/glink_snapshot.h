#pragma once
#include <stddef.h>
#include <stdint.h>
#include "glink_decide.h"
#include "glink_messages.h"

// What the UI task publishes for the Bluetooth task (docs/GLINK-PROTOCOL.md 5.3, 5.4): the screen class, the periodInfo
// fields and the profiles. Reads (periodInfo, grindProfile get) are served from this copy on the Bluetooth task, which
// never touches the controllers, the profiles or NVS. Pure and host-tested.
struct GlinkSnapshot {
    GlinkScreen screen = GlinkScreen::OTHER;
    GlinkPeriodInfo period;
    GlinkProfileEntry profiles[GLINK_PROFILE_COUNT_MAX];  // index = profile; entry 3 valid only if profile3_exists
    bool profile3_exists = false;
    bool has_result = false;      // a grind finished since boot
    GlinkResultInfo last_result;  // valid only if has_result
};

// grindProfile get from a snapshot: the state rules of 7.1 (UPDATING), then all profiles or one. Returns the message
// length, or 0 with `failure` set (also when the reply would not fit in `cap`).
size_t glink_serve_get(const GlinkSnapshot& snapshot, const GlinkRequest& request, char* out, size_t cap,
                       GlinkFailure& failure);

// grindResult get from a snapshot (5.6): the state rules of 7.1 (UPDATING), NOT_FOUND before any result since boot,
// else the last result. Returns the message length, or 0 with `failure` set.
size_t glink_serve_result_get(const GlinkSnapshot& snapshot, char* out, size_t cap, GlinkFailure& failure);
