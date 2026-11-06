#pragma once

#include <stdint.h>
#include "../../bluetooth/glink_decide.h"
#include "../../bluetooth/glink_mailbox.h"
#include "../../bluetooth/glink_screen.h"
#include "../../bluetooth/glink_snapshot.h"
#include "../../network/portal_guard.h"
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
//  - Start (grindControl start): decided like the web panel's start (glink_decide_start on the real screen state, which
//    includes the start block), then a GLINK_START_LEAD_IN_MS "Remote start" overlay that a touch, its STOP button or a
//    stop cancels (portal_lead_in_* through glink_lead_in_step). The lead-in is exclusive with the web panel's: each
//    counts the other's as in flight. At GO the decision is made again and start_glink_grind() runs the shared start
//    body (weight mode, start_block() checked again inside the grind controller). Nothing here bypasses those checks.
//  - Stop is never queued: the Bluetooth task calls portal_stop_now() at once; this controller only notices the counter
//    portal_command_gate raises, to cancel a lead-in.
//  - Result: a change of GrindController::result_count() is read through the seam (read_last_result), mapped to the wire
//    result (code, origin, ...) and published in the snapshot, whoever started the grind; the Bluetooth task broadcasts it.
// A link that drops changes nothing here: a lead-in and a grind in progress go on. Decisions and conversions are pure
// functions in bluetooth/glink_*.
class GlinkUIController {
public:
    explicit GlinkUIController(UIManager* manager);
    ~GlinkUIController();

    void update();
    // A remote start (BLE) is in its touch-to-cancel lead-in; the web panel counts it as in flight
    bool lead_in_active() const { return lead_in_.active; }

private:
    void serve_mailbox();
    void handle_request(const GlinkRequest& request);
    GlinkMailboxResult apply_update(const GlinkRequest& request, GlinkScreen screen, uint32_t now_ms);
    GlinkMailboxResult apply_select(const GlinkRequest& request, GlinkScreen screen, uint32_t now_ms);
    bool store_dose_profile(const GlinkProfileEntry& wanted, const GlinkProfileState& before);
    GlinkProfileState dose_profile_state() const;
    GlinkScreen classify_screen(bool ignore_own_lead_in = false) const;
    GlinkMailboxResult apply_start(const GlinkRequest& request, GlinkScreen screen);
    GlinkStartInputs gather_start_inputs(int profile, bool has_expect, int32_t expect_tenths) const;
    void run_lead_in();
    void end_lead_in();
    void start_after_lead_in();
    void watch_result();
    void fill_lead_in_period(GlinkPeriodInputs* in, uint32_t now_ms) const;
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
    PortalLeadIn lead_in_;              // BLE remote start waiting for its touch-to-cancel window
    int lead_in_profile_ = 0;
    bool lead_in_has_expect_ = false;   // expectDose of the start request, checked again at GO
    int32_t lead_in_expect_tenths_ = 0;
    uint32_t lead_in_shown_s_ = 0;      // seconds last written to the overlay text
    uint32_t stop_seen_ = 0;            // portal_command_gate.stop_seq() as last handled
    uint32_t seen_result_count_ = 0;    // GrindController::result_count() as last published
    uint32_t ble_session_id_ = 0;       // grind session started by a GLink start (origin BLE)
    GlinkSnapshot snapshot_;            // member, not a local: the UI task stack is shared with LVGL
};
