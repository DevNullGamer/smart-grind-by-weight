#pragma once
#include <stdint.h>

// Pure state machine for the on-device Accept prompt (no Arduino/ESP dependencies, unit tested on the host).
// The HTTP task files a request and waits; the UI task answers it. Time is injected (millis()); all time
// maths is wrap-safe. Thread safety is the job of the wrapper in update_approval_gate.*.

enum class ApprovalState : uint8_t {
    IDLE,       // nothing filed
    PENDING,    // request filed, waiting for the user
    ACCEPTED,   // user tapped INSTALL
    DECLINED,   // user tapped DECLINE (starts the back-off)
    EXPIRED,    // nobody answered within the timeout
    CANCELLED,  // withdrawn (Receive switched off, WiFi forgotten, ...)
    UI_BUSY     // the screen cannot show the prompt right now
};

struct UpdateApproval {
    ApprovalState state = ApprovalState::IDLE;
    uint32_t requested_at_ms = 0;
    uint32_t timeout_ms = 0;
    uint32_t backoff_ms = 0;        // length of the back-off a decline starts (set by approval_request)
    bool backoff_active = false;
    uint32_t backoff_until_ms = 0;
};

enum class ApprovalRequestResult : uint8_t {
    OK,
    ALREADY_PENDING,  // a prompt is already waiting for an answer
    BACKOFF           // a decline happened recently
};

// Files a request. Any finished (non-PENDING) state is overwritten: the waiting side always clears it, so a
// stale one must not block new requests. backoff_ms is remembered for a later decline.
ApprovalRequestResult approval_request(UpdateApproval* a, uint32_t now_ms, uint32_t timeout_ms, uint32_t backoff_ms);

// The following only act on a PENDING request and return false otherwise (a late answer never overwrites
// an expiry or a cancel).
bool approval_resolve(UpdateApproval* a, bool accept, uint32_t now_ms);  // a decline starts the back-off
bool approval_resolve_busy(UpdateApproval* a);
bool approval_cancel(UpdateApproval* a);

// Applies the expiry and returns the current state.
ApprovalState approval_poll(UpdateApproval* a, uint32_t now_ms);
// Back to IDLE (the back-off stays).
void approval_clear(UpdateApproval* a);

// Milliseconds until new requests are accepted again (0 when no back-off); an elapsed back-off is dropped.
uint32_t approval_backoff_remaining_ms(UpdateApproval* a, uint32_t now_ms);
// Milliseconds until a PENDING request expires (0 when not pending).
uint32_t approval_remaining_ms(const UpdateApproval* a, uint32_t now_ms);
