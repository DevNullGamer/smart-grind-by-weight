#pragma once

#include <stdint.h>
#include "../../network/portal_api.h"
#include "../../network/portal_apply.h"
#include "../../network/portal_guard.h"
#include "../../network/portal_snapshot.h"
#include "../../system/state_machine.h"

class UIManager;

// UI side of the web panel (docs/PORTAL-API.md), polled from UIManager::update() on the UI task. The HTTP task
// never touches LVGL, the controllers, the profiles or NVS; it talks to this controller only through
// portal_command_gate:
//  - Snapshot: while the Web interface is on, republishes the status every PORTAL_STATUS_PUBLISH_MS and the
//    stored settings (settings, grind mode, profile targets, device name) every PORTAL_VALUES_REFRESH_MS and
//    right after a write; GET /api/status, /api/settings read that copy.
//  - Mailbox: takes a PATCH /api/settings request, decides it (portal_write_decide: READY or MENU only, never
//    busy), applies it through the handlers the touchscreen uses (UiSettings, ProfileController, WiFiManager;
//    order and failure rules in portal_apply.h) and then updates what is on screen: brightness, auto actions,
//    the READY tab values and grind mode, the open menu page. A command (tare, start) is decided by
//    portal_command_decide and started like the Scale page's TARE / the READY screen's START button. A start
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

private:
    // PortalApplyTarget: the writes of a PATCH, through the handlers of the touchscreen
    bool write_field(size_t index, float value) override;
    bool write_device_name(const char* name, bool* changed) override;

    void serve_mailbox();
    void handle_patch(const PortalPatch& patch);
    void handle_command(const PortalCommandRequest& command);
    void handle_stop();
    bool run_tare();
    void begin_remote_start(int profile);
    void run_lead_in();
    void end_lead_in();
    void lead_in_text(char* out, size_t cap, uint32_t remaining_s) const;
    void apply_to_screen(uint32_t written_mask);
    void publish_snapshot(uint32_t now_ms, bool reload_values);
    void load_values(PortalValues* out) const;
    PortalStatusInputs gather_status() const;
    PortalDecisionInput gather_decision() const;
    bool grinder_busy() const;

    UIManager* ui_manager_;
    bool attached_ = false;
    uint32_t stop_seen_ = 0;         // portal_command_gate.stop_seq() as last handled
    bool web_was_enabled_ = false;
    UIState last_state_ = UIState::READY;
    bool publish_due_ = false;       // a write just finished: publish at once
    uint32_t last_publish_ms_ = 0;
    uint32_t values_loaded_ms_ = 0;
    PortalValues values_;
    bool values_valid_ = false;
    PortalLeadIn lead_in_;           // remote start waiting for its touch-to-cancel window
    int lead_in_profile_ = 0;
    uint32_t lead_in_shown_s_ = 0;   // seconds last written to the overlay text
};
