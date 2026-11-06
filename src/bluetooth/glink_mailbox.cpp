#include "glink_mailbox.h"

namespace {

bool elapsed_at_least(uint32_t since_ms, uint32_t now_ms, uint32_t limit_ms) {
    return static_cast<uint32_t>(now_ms - since_ms) >= limit_ms;
}

}  // namespace

bool glink_mailbox_submit(GlinkMailbox* box, const GlinkRequest& request, uint32_t now_ms) {
    if (box->state != GlinkMailboxState::IDLE) return false;
    box->request = request;
    box->abandoned = false;
    box->queued_at_ms = now_ms;
    box->state = GlinkMailboxState::QUEUED;
    return true;
}

GlinkMailboxWait glink_mailbox_poll(GlinkMailbox* box, uint32_t now_ms, uint32_t pickup_timeout_ms,
                                    uint32_t run_timeout_ms, GlinkMailboxResult* out) {
    switch (box->state) {
        case GlinkMailboxState::IDLE: return GlinkMailboxWait::TIMEOUT;
        case GlinkMailboxState::QUEUED:
            if (!elapsed_at_least(box->queued_at_ms, now_ms, pickup_timeout_ms)) return GlinkMailboxWait::WAITING;
            box->state = GlinkMailboxState::IDLE;  // withdrawn: nothing happened, the UI task cannot take it any more
            return GlinkMailboxWait::TIMEOUT;
        case GlinkMailboxState::TAKEN:
            if (!elapsed_at_least(box->taken_at_ms, now_ms, run_timeout_ms)) return GlinkMailboxWait::WAITING;
            box->abandoned = true;  // complete() will drop the answer
            return GlinkMailboxWait::TIMEOUT;
        case GlinkMailboxState::DONE:
            if (out) *out = box->result;
            box->state = GlinkMailboxState::IDLE;
            return GlinkMailboxWait::DONE;
    }
    return GlinkMailboxWait::TIMEOUT;
}

bool glink_mailbox_take(GlinkMailbox* box, GlinkRequest* out, uint32_t now_ms, uint32_t pickup_timeout_ms) {
    if (box->state != GlinkMailboxState::QUEUED) return false;
    if (elapsed_at_least(box->queued_at_ms, now_ms, pickup_timeout_ms)) return false;
    if (out) *out = box->request;
    box->taken_at_ms = now_ms;
    box->state = GlinkMailboxState::TAKEN;
    return true;
}

void glink_mailbox_complete(GlinkMailbox* box, const GlinkMailboxResult& result) {
    if (box->state != GlinkMailboxState::TAKEN) return;
    if (box->abandoned) {
        box->abandoned = false;
        box->state = GlinkMailboxState::IDLE;
        return;
    }
    box->result = result;
    box->state = GlinkMailboxState::DONE;
}

bool glink_snapshot_fresh(bool published, uint32_t published_ms, uint32_t now_ms, uint32_t stale_ms) {
    return published && static_cast<uint32_t>(now_ms - published_ms) < stale_ms;
}
