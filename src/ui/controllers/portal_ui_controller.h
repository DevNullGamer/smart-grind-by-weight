#pragma once

#include <stdint.h>
#include "../../network/portal_api.h"
#include "../../network/portal_apply.h"
#include "../../network/portal_guard.h"
#include "../../network/portal_insights.h"
#include "../../network/portal_snapshot.h"
#include "../../logging/grind_history.h"
#include "../../system/state_machine.h"

class UIManager;

// UI side of the web panel (docs/PORTAL-API.md), polled from UIManager::update() on the UI task. The HTTP task
// never touches LVGL, the controllers, the profiles or NVS; it talks to this controller only through
// portal_command_gate:
//  - Snapshot: while the Web interface is on, republishes the status every PORTAL_STATUS_PUBLISH_MS and the
//    stored settings (settings, grind mode, profile targets, device name) every PORTAL_VALUES_REFRESH_MS and
//    right after a write; GET /api/status, /api/settings read that copy.
//    The lifetime statistics (GET /api/stats) are copied from statistics_manager every PORTAL_STATS_REFRESH_MS and on a
//    screen state change into their own slot; only this task calls the statistics getters.
//    The recent grinds (history ring, filled by the file-IO task) are copied by gather_history() into their own slot as
//    soon as the ring or its flags changed (checked every PORTAL_STATUS_PUBLISH_MS) and otherwise every
//    PORTAL_HISTORY_REFRESH_MS; a new grind also refreshes the statistics slot. The UI task never reads the history
//    files and the HTTP task never reads the ring.
//    The health numbers (GET /api/diagnostics) are gathered by gather_diagnostics() every PORTAL_DIAGNOSTICS_REFRESH_MS
//    into their own slot; only this task calls the load cell, WiFi and Bluetooth getters.
//  - Mailbox: takes a PATCH /api/settings request, decides it (portal_write_decide: READY or MENU only, never
//    busy), applies it through the handlers the touchscreen uses (UiSettings, ProfileController, WiFiManager;
//    order and failure rules in portal_apply.h) and then updates what is on screen: brightness, auto actions,
//    the READY tab values and grind mode, the open menu page. A command (tare, start) is decided by
//    portal_command_decide and started like the Scale page's TARE / the READY screen's START button / the Motor Test
//    RUN button (motor_test: no dialog and no lead-in, MenuUIController::begin_motor_test). A start
//    answers 202 at once and first runs a PORTAL_LEAD_IN_MS "Remote start" overlay that a touch, its STOP button
//    or a web stop cancels; the lead-in counts as in flight (other commands and writes are refused) and the
//    start is decided again when it ends.
//  - Stop: not a mailbox request (it must work while the UI task is slow). The motor and the grind are stopped on
//    the HTTP task and the grind control task (portal_stop.h); here only the counter is noticed, to cancel an
//    autotune run, which this task drives.
// Decisions and conversions are pure functions in network/portal_*.
class PortalUIController : private PortalApplyTarget {
public:
    explicit PortalUIController(UIManager* manager);
    ~PortalUIController();

    void update();
    // A remote start (web panel) is in its touch-to-cancel lead-in; GLink reports it as STARTING / refuses writes
    bool lead_in_active() const { return lead_in_.active; }
    // For GLink's periodInfo while STARTING: the web panel's lead-in and the profile it will grind
    const PortalLeadIn& lead_in_state() const { return lead_in_; }
    int lead_in_profile() const { return lead_in_profile_; }
    // Grind session of the last remote start the web panel made (0 = none); GLink reports origin WEB for its result
    uint32_t web_session_id() const { return web_session_id_; }

private:
    // PortalApplyTarget: the writes of a PATCH, through the handlers of the touchscreen
    bool write_field(size_t index, float value) override;
    bool write_device_name(const char* name, bool* changed) override;

    void serve_mailbox();
    void handle_patch(const PortalPatch& patch);
    void handle_command(const PortalCommandRequest& command);
    void handle_stop();
    bool run_tare();
    PortalError run_motor_test();
    void begin_remote_start(int profile);
    void run_lead_in();
    void end_lead_in();
    void lead_in_text(char* out, size_t cap, uint32_t remaining_s) const;
    void apply_to_screen(uint32_t written_mask);
    void publish_snapshot(uint32_t now_ms, bool reload_values);
    void load_values(PortalValues* out) const;
    PortalStatusInputs gather_status() const;
    PortalStats gather_stats() const;
    // The health numbers of GET /api/diagnostics, read on the UI task from the same getters the Diagnostics, WiFi and
    // Bluetooth pages use (no logging, no allocation)
    PortalDiagnostics gather_diagnostics() const;
    // Copies the history ring into history_snap_ / history_view_ (one short spinlock section, no file access)
    void gather_history();
    // Publishes the history slot when the ring changed, when force is set or when it is due; true when the ring changed
    bool refresh_history(uint32_t now_ms, bool force);
    PortalDecisionInput gather_decision() const;
    bool grinder_busy() const;

    UIManager* ui_manager_;
    bool attached_ = false;
    uint32_t stop_seen_ = 0;         // portal_command_gate.stop_seq() as last handled
    bool web_was_enabled_ = false;
    UIState last_state_ = UIState::READY;
    bool publish_due_ = false;       // a write just finished: publish at once
    uint32_t last_publish_ms_ = 0;
    uint32_t last_stats_ms_ = 0;     // statistics snapshot, refreshed on its own (slower) schedule
    uint32_t last_diagnostics_ms_ = 0;   // diagnostics snapshot, every PORTAL_DIAGNOSTICS_REFRESH_MS
    uint32_t last_history_check_ms_ = 0;
    uint32_t last_history_publish_ms_ = 0;
    bool history_seen_ = false;      // history_snap_ holds a copy that was published
    GrindHistorySnapshot history_snap_;  // members, not locals: the UI task stack is shared with LVGL
    PortalHistory history_view_;
    uint32_t values_loaded_ms_ = 0;
    PortalValues values_;
    bool values_valid_ = false;
    PortalLeadIn lead_in_;           // remote start waiting for its touch-to-cancel window
    int lead_in_profile_ = 0;
    uint32_t web_session_id_ = 0;
    uint32_t lead_in_shown_s_ = 0;   // seconds last written to the overlay text
};
