#pragma once
#include <stdint.h>
#include "portal_api.h"

// Pure state machine of the UI-task mailbox of the web panel (no Arduino/ESP dependencies, unit tested on the
// host). The HTTP task files ONE write request and waits; the UI task takes it, applies it through the same
// handlers as the touchscreen and posts the result. Time is injected (millis()); all time maths is wrap-safe.
// Thread safety is the job of the wrapper in portal_command_gate.*.
//
//   IDLE --submit--> QUEUED --take--> TAKEN --complete--> DONE --collect--> IDLE
//
// A request the UI task has not taken by the pickup timeout is withdrawn (nothing happened). One it has taken
// cannot be withdrawn: after the run timeout the waiting side abandons it, the mailbox stays busy
// (COMMAND_IN_FLIGHT for new requests) until the UI task completes it, then returns to IDLE by itself.

enum class PortalMailboxState : uint8_t { IDLE, QUEUED, TAKEN, DONE };

struct PortalMailboxResult {
    PortalError error = PortalError::NONE;
    PortalValues applied;               // the stored state after the write (also on STORE_FAILED: what did get applied)
    uint32_t adjusted_mask = 0;         // PortalPatch::adjusted
    bool device_name_changed = false;   // the reply carries the new address
};

struct PortalMailbox {
    PortalMailboxState state = PortalMailboxState::IDLE;
    bool abandoned = false;             // the waiting side gave up on a TAKEN request
    uint32_t queued_at_ms = 0;
    uint32_t taken_at_ms = 0;
    PortalPatch patch;                  // the request (QUEUED, TAKEN)
    PortalMailboxResult result;         // the answer (DONE)
};

// HTTP side: COMMAND_IN_FLIGHT unless the mailbox is IDLE, else files the patch (QUEUED) and returns NONE.
PortalError portal_mailbox_submit(PortalMailbox* box, const PortalPatch& patch, uint32_t now_ms);

enum class PortalMailboxWait : uint8_t {
    WAITING,  // keep polling
    DONE,     // *out holds the answer and the mailbox is IDLE again
    TIMEOUT   // answer 503 ui_busy: a QUEUED request was withdrawn (mailbox IDLE), a TAKEN one is abandoned
};
PortalMailboxWait portal_mailbox_poll(PortalMailbox* box, uint32_t now_ms, uint32_t pickup_timeout_ms,
                                      uint32_t run_timeout_ms, PortalMailboxResult* out);

// UI side: QUEUED -> TAKEN and copies the request (false when nothing is queued).
bool portal_mailbox_take(PortalMailbox* box, PortalPatch* out, uint32_t now_ms);
// UI side: TAKEN -> DONE (or straight to IDLE when the waiting side has abandoned it). Ignored in any other state.
void portal_mailbox_complete(PortalMailbox* box, const PortalMailboxResult& result);

// True while a request is queued, being handled or waiting to be collected
inline bool portal_mailbox_busy(const PortalMailbox& box) { return box.state != PortalMailboxState::IDLE; }

// A snapshot published at published_ms is still good at now_ms (never published = not fresh; wrap-safe)
bool portal_snapshot_fresh(bool published, uint32_t published_ms, uint32_t now_ms, uint32_t stale_ms);
