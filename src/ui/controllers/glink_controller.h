#pragma once

#include <stdint.h>
#include "../../bluetooth/glink_decide.h"
#include "../../bluetooth/glink_mailbox.h"
#include "../../bluetooth/glink_snapshot.h"
#include "../../system/state_machine.h"

class UIManager;

// UI side of GLink (docs/GLINK-PROTOCOL.md), polled from UIManager::update() on the UI task. The Bluetooth task never
// touches LVGL, the controllers, the profiles or NVS; it talks to this controller only through glink_command_gate:
//  - Snapshot: while a client is linked, republishes the screen class, the periodInfo fields and the four profiles every
//    BLE_GLINK_SNAPSHOT_PUBLISH_MS, on a screen change and right after a write; periodInfo and grindProfile get are
//    served from that copy. profileRev counts every change of a stored dose, grind size, unit or of the existence of the
//    Profile Dose, whatever its origin (found by comparing consecutive snapshots).
//  - Mailbox: takes one grindProfile update or select, decides it (glink_decide_* on the real screen state), applies it
//    through ProfileController (compare-first: an unchanged value writes no flash; stored changes at most once a second),
//    refreshes the READY page, publishes a fresh snapshot and only then answers.
// Start, stop and the result broadcast are step 6b. Decisions and conversions are pure functions in bluetooth/glink_*.
class GlinkUIController {
public:
    explicit GlinkUIController(UIManager* manager);
    ~GlinkUIController();

    void update();

private:
    void serve_mailbox();
    void handle_request(const GlinkRequest& request);
    GlinkMailboxResult apply_update(const GlinkRequest& request, GlinkScreen screen, uint32_t now_ms);
    GlinkMailboxResult apply_select(const GlinkRequest& request, GlinkScreen screen, uint32_t now_ms);
    bool store_dose_profile(const GlinkProfileEntry& wanted, const GlinkProfileState& before);
    GlinkProfileState dose_profile_state() const;
    GlinkScreen classify_screen() const;
    void publish_snapshot(uint32_t now_ms);
    void fill_profiles(GlinkSnapshot* out) const;
    void count_profile_changes(const GlinkSnapshot& now);

    UIManager* ui_manager_;
    bool attached_ = false;
    bool linked_ = false;               // a client was connected in the last frame
    UIState last_state_ = UIState::READY;
    uint32_t last_publish_ms_ = 0;
    GlinkWriteLimiter limiter_;
    uint32_t profile_rev_ = 0;
    bool profiles_seen_ = false;        // last_profiles_ holds a baseline
    GlinkProfileEntry last_profiles_[GLINK_PROFILE_COUNT_MAX];
    bool last_exists_ = false;
    GlinkSnapshot snapshot_;            // member, not a local: the UI task stack is shared with LVGL
};
