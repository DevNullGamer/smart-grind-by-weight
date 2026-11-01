#include "network/portal_mailbox.h"

PortalError portal_mailbox_submit(PortalMailbox* box, const PortalPatch& patch, uint32_t now_ms) {
    if (box->state != PortalMailboxState::IDLE) return PortalError::COMMAND_IN_FLIGHT;
    box->patch = patch;
    box->result = PortalMailboxResult();
    box->abandoned = false;
    box->queued_at_ms = now_ms;
    box->state = PortalMailboxState::QUEUED;
    return PortalError::NONE;
}

PortalMailboxWait portal_mailbox_poll(PortalMailbox* box, uint32_t now_ms, uint32_t pickup_timeout_ms,
                                      uint32_t run_timeout_ms, PortalMailboxResult* out) {
    switch (box->state) {
        case PortalMailboxState::DONE:
            if (out) *out = box->result;
            box->state = PortalMailboxState::IDLE;
            box->abandoned = false;
            return PortalMailboxWait::DONE;
        case PortalMailboxState::QUEUED:
            if (static_cast<uint32_t>(now_ms - box->queued_at_ms) >= pickup_timeout_ms) {
                box->state = PortalMailboxState::IDLE;  // never taken: nothing was applied
                return PortalMailboxWait::TIMEOUT;
            }
            return PortalMailboxWait::WAITING;
        case PortalMailboxState::TAKEN:
            if (static_cast<uint32_t>(now_ms - box->taken_at_ms) >= run_timeout_ms) {
                box->abandoned = true;  // the UI task completes it later; nobody collects the answer
                return PortalMailboxWait::TIMEOUT;
            }
            return PortalMailboxWait::WAITING;
        case PortalMailboxState::IDLE:
            break;
    }
    return PortalMailboxWait::TIMEOUT;  // polling without a request: nothing will ever arrive
}

bool portal_mailbox_take(PortalMailbox* box, PortalPatch* out, uint32_t now_ms) {
    if (box->state != PortalMailboxState::QUEUED) return false;
    if (out) *out = box->patch;
    box->taken_at_ms = now_ms;
    box->state = PortalMailboxState::TAKEN;
    return true;
}

void portal_mailbox_complete(PortalMailbox* box, const PortalMailboxResult& result) {
    if (box->state != PortalMailboxState::TAKEN) return;
    if (box->abandoned) {
        box->state = PortalMailboxState::IDLE;
        box->abandoned = false;
        return;
    }
    box->result = result;
    box->state = PortalMailboxState::DONE;
}

bool portal_snapshot_fresh(bool published, uint32_t published_ms, uint32_t now_ms, uint32_t stale_ms) {
    return published && static_cast<uint32_t>(now_ms - published_ms) < stale_ms;
}
