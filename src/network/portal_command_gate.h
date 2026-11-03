#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include "portal_mailbox.h"

/**
 * PortalCommandGate - thread-safe hand-over between the HTTP task and the UI task for the web panel
 * (docs/PORTAL-API.md, "Writes and commands"). Modelled on UpdateApprovalGate: the pure state machine lives in
 * portal_mailbox.*, this wrapper only adds the lock, the clock and the wait loop. All critical sections are a
 * few struct copies (spinlock, no blocking call inside).
 *
 *  - Reads: the UI task publishes a snapshot (values, status) every PORTAL_STATUS_PUBLISH_MS; the HTTP task
 *    copies it. The HTTP task never calls LVGL, the controllers, UiSettings or NVS.
 *  - Writes and commands: the HTTP task files one PATCH or command and waits (submit_patch/submit_command);
 *    the UI task takes it, applies it and completes it. One request is in flight at a time.
 *  - Stop: never queued (see request_stop()).
 *
 * Until the UI controller calls attach_ui(true) nothing can be answered, so reads and writes fail fast with
 * UI_BUSY.
 */
class PortalCommandGate {
public:
    // ---- HTTP task ----
    // False when no fresh snapshot exists (UI task not running or stuck): answer 503 ui_busy.
    bool read_snapshot(PortalValues* values, PortalStatus* status);
    // Files the patch and waits up to the pickup/run timeouts. NONE: *result holds the UI task's answer (its
    // result.error may itself be a refusal or STORE_FAILED). Otherwise the refusal (COMMAND_IN_FLIGHT, UI_BUSY).
    PortalError submit_patch(const PortalPatch& patch, PortalMailboxResult* result);
    // Same for a command other than stop (tare, start, motor test): result->error is the UI task's decision.
    PortalError submit_command(const PortalCommandRequest& command, PortalMailboxResult* result);
    // Stop does not use the mailbox: it only bumps this counter (lock-free), so it works while a write is in
    // flight and does not wait for the UI task. The motor itself is stopped by portal_stop_now().
    void request_stop() { stop_seq_.fetch_add(1); }

    // ---- UI task ----
    bool take_patch(PortalPatch* out);
    bool take_command(PortalCommandRequest* out);
    uint32_t stop_seq() const { return stop_seq_.load(); }
    void complete(const PortalMailboxResult& result);
    void publish(const PortalValues& values, const PortalStatus& status);
    void attach_ui(bool attached);
    bool ui_attached() const { return ui_attached_; }

private:
    // Files are done under the lock by the callers; this waits for the answer (pickup/run timeouts)
    PortalError wait_for_answer(PortalMailboxResult* result);

    std::atomic<uint32_t> stop_seq_{0};
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    PortalMailbox mailbox_;
    PortalValues values_;
    PortalStatus status_;
    bool published_ = false;
    uint32_t published_ms_ = 0;
    volatile bool ui_attached_ = false;
};

extern PortalCommandGate portal_command_gate;
