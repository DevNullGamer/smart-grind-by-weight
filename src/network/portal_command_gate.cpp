#include "portal_command_gate.h"

PortalCommandGate portal_command_gate;

namespace {

// RAII spinlock section
struct Section {
    portMUX_TYPE* mux;
    explicit Section(portMUX_TYPE* m) : mux(m) { portENTER_CRITICAL(mux); }
    ~Section() { portEXIT_CRITICAL(mux); }
};

}  // namespace

bool PortalCommandGate::read_snapshot(PortalValues* values, PortalStatus* status) {
    Section section(&mux_);
    if (!ui_attached_ || !portal_snapshot_fresh(published_, published_ms_, millis(), PORTAL_SNAPSHOT_STALE_MS)) {
        return false;
    }
    if (values) *values = values_;
    if (status) *status = status_;
    return true;
}

bool PortalCommandGate::read_stats(PortalStats* stats) {
    Section section(&stats_mux_);
    if (!ui_attached_ ||
        !portal_snapshot_fresh(stats_published_, stats_published_ms_, millis(), PORTAL_STATS_STALE_MS)) {
        return false;
    }
    if (stats) *stats = stats_;
    return true;
}

PortalError PortalCommandGate::submit_patch(const PortalPatch& patch, PortalMailboxResult* result) {
    if (!ui_attached_) return PortalError::UI_BUSY;
    {
        Section section(&mux_);
        const PortalError filed = portal_mailbox_submit(&mailbox_, patch, millis());
        if (filed != PortalError::NONE) return filed;
    }
    return wait_for_answer(result);
}

PortalError PortalCommandGate::submit_command(const PortalCommandRequest& command, PortalMailboxResult* result) {
    if (!ui_attached_) return PortalError::UI_BUSY;
    {
        Section section(&mux_);
        const PortalError filed = portal_mailbox_submit_command(&mailbox_, command, millis());
        if (filed != PortalError::NONE) return filed;
    }
    return wait_for_answer(result);
}

PortalError PortalCommandGate::wait_for_answer(PortalMailboxResult* result) {
    while (true) {
        PortalMailboxWait wait;
        {
            Section section(&mux_);
            wait = portal_mailbox_poll(&mailbox_, millis(), PORTAL_PICKUP_TIMEOUT_MS, PORTAL_RUN_TIMEOUT_MS, result);
        }
        if (wait == PortalMailboxWait::DONE) return PortalError::NONE;
        if (wait == PortalMailboxWait::TIMEOUT) return PortalError::UI_BUSY;
        vTaskDelay(pdMS_TO_TICKS(PORTAL_WAIT_POLL_MS));
    }
}

bool PortalCommandGate::take_patch(PortalPatch* out) {
    Section section(&mux_);
    return portal_mailbox_take(&mailbox_, out, millis());
}

bool PortalCommandGate::take_command(PortalCommandRequest* out) {
    Section section(&mux_);
    return portal_mailbox_take_command(&mailbox_, out, millis());
}

void PortalCommandGate::complete(const PortalMailboxResult& result) {
    Section section(&mux_);
    portal_mailbox_complete(&mailbox_, result);
}

void PortalCommandGate::publish(const PortalValues& values, const PortalStatus& status) {
    Section section(&mux_);
    values_ = values;
    status_ = status;
    published_ = true;
    published_ms_ = millis();
}

void PortalCommandGate::publish_stats(const PortalStats& stats) {
    Section section(&stats_mux_);
    stats_ = stats;
    stats_published_ = true;
    stats_published_ms_ = millis();
}

void PortalCommandGate::attach_ui(bool attached) {
    {
        Section section(&mux_);
        ui_attached_ = attached;
        if (!attached) published_ = false;  // a snapshot nobody refreshes any more is stale
    }
    if (!attached) {
        Section section(&stats_mux_);
        stats_published_ = false;
    }
}
