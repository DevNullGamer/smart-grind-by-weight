#include "update_approval.h"

namespace {

// Unsigned distance from now to a deadline; 0 when the deadline has passed. A deadline that is "ahead" by more
// than max_ahead cannot be a real one (the clock wrapped since it passed), so it counts as passed.
uint32_t remaining_until(uint32_t deadline_ms, uint32_t now_ms, uint32_t max_ahead_ms) {
    const uint32_t ahead = deadline_ms - now_ms;
    if (ahead == 0 || ahead > max_ahead_ms) return 0;
    return ahead;
}

}  // namespace

uint32_t approval_backoff_remaining_ms(UpdateApproval* a, uint32_t now_ms) {
    if (!a->backoff_active) return 0;
    const uint32_t remaining = remaining_until(a->backoff_until_ms, now_ms, a->backoff_ms);
    if (remaining == 0) a->backoff_active = false;
    return remaining;
}

uint32_t approval_remaining_ms(const UpdateApproval* a, uint32_t now_ms) {
    if (a->state != ApprovalState::PENDING) return 0;
    const uint32_t elapsed = now_ms - a->requested_at_ms;
    return elapsed >= a->timeout_ms ? 0 : a->timeout_ms - elapsed;
}

ApprovalRequestResult approval_request(UpdateApproval* a, uint32_t now_ms, uint32_t timeout_ms, uint32_t backoff_ms) {
    if (a->state == ApprovalState::PENDING) return ApprovalRequestResult::ALREADY_PENDING;
    a->backoff_ms = backoff_ms;
    if (approval_backoff_remaining_ms(a, now_ms) > 0) return ApprovalRequestResult::BACKOFF;
    a->state = ApprovalState::PENDING;
    a->requested_at_ms = now_ms;
    a->timeout_ms = timeout_ms;
    return ApprovalRequestResult::OK;
}

bool approval_resolve(UpdateApproval* a, bool accept, uint32_t now_ms) {
    if (approval_poll(a, now_ms) != ApprovalState::PENDING) return false;  // too late: it expired first
    if (accept) {
        a->state = ApprovalState::ACCEPTED;
    } else {
        a->state = ApprovalState::DECLINED;
        if (a->backoff_ms > 0) {
            a->backoff_active = true;
            a->backoff_until_ms = now_ms + a->backoff_ms;
        }
    }
    return true;
}

bool approval_resolve_busy(UpdateApproval* a) {
    if (a->state != ApprovalState::PENDING) return false;
    a->state = ApprovalState::UI_BUSY;
    return true;
}

bool approval_cancel(UpdateApproval* a) {
    if (a->state != ApprovalState::PENDING) return false;
    a->state = ApprovalState::CANCELLED;
    return true;
}

ApprovalState approval_poll(UpdateApproval* a, uint32_t now_ms) {
    if (a->state == ApprovalState::PENDING && approval_remaining_ms(a, now_ms) == 0) {
        a->state = ApprovalState::EXPIRED;
    }
    return a->state;
}

void approval_clear(UpdateApproval* a) {
    a->state = ApprovalState::IDLE;
}
