#include "glink_gate.h"
#include "../config/bluetooth.h"

GlinkCommandGate glink_command_gate;

namespace {

// RAII spinlock section
struct Section {
    portMUX_TYPE* mux;
    explicit Section(portMUX_TYPE* m) : mux(m) { portENTER_CRITICAL(mux); }
    ~Section() { portEXIT_CRITICAL(mux); }
};

}  // namespace

bool GlinkCommandGate::read_snapshot(GlinkSnapshot* out) {
    Section section(&mux_);
    if (!ui_attached_ || !glink_snapshot_fresh(published_, published_ms_, millis(), BLE_GLINK_SNAPSHOT_STALE_MS)) {
        return false;
    }
    if (out) *out = snapshot_;
    return true;
}

bool GlinkCommandGate::submit(const GlinkRequest& request) {
    Section section(&mux_);
    if (!ui_attached_) return false;
    return glink_mailbox_submit(&mailbox_, request, millis());
}

GlinkMailboxWait GlinkCommandGate::poll(GlinkMailboxResult* out) {
    Section section(&mux_);
    return glink_mailbox_poll(&mailbox_, millis(), GLINK_UI_REPLY_TIMEOUT_MS, BLE_GLINK_UI_RUN_TIMEOUT_MS, out);
}

bool GlinkCommandGate::take(GlinkRequest* out) {
    Section section(&mux_);
    return glink_mailbox_take(&mailbox_, out, millis(), GLINK_UI_REPLY_TIMEOUT_MS);
}

bool GlinkCommandGate::request_queued() {
    Section section(&mux_);
    return mailbox_.state == GlinkMailboxState::QUEUED;
}

void GlinkCommandGate::complete(const GlinkMailboxResult& result) {
    Section section(&mux_);
    glink_mailbox_complete(&mailbox_, result);
}

void GlinkCommandGate::publish(const GlinkSnapshot& snapshot) {
    Section section(&mux_);
    snapshot_ = snapshot;
    published_ = true;
    published_ms_ = millis();
}

void GlinkCommandGate::attach_ui(bool attached) {
    Section section(&mux_);
    ui_attached_ = attached;
    if (!attached) published_ = false;  // a snapshot nobody refreshes any more is stale
}
