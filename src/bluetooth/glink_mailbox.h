#pragma once
#include <stdint.h>
#include "glink_messages.h"

// Pure state machine of the one-request mailbox between the Bluetooth task and the UI task (docs/GLINK-PROTOCOL.md 4.1
// step 8 and 7.4). No Arduino/ESP dependencies, unit tested on the host; time is injected and wrap-safe. Locking is the
// job of the wrapper in glink_gate.*. Same shape as network/portal_mailbox.*, but the Bluetooth task never waits: it
// files the request and polls on its next ticks.
//
//   IDLE --submit--> QUEUED --take--> TAKEN --complete--> DONE --poll--> IDLE
//
// A request is claimed by exactly one side. If the UI task takes it before the pickup deadline it is applied as decided,
// even if the client has gone meanwhile. If the deadline passes first, the poll withdraws it (UI_BUSY) and the UI task can
// no longer take it. A request the UI task took but did not finish within the run timeout is answered UI_BUSY by the poll
// and abandoned: the mailbox stays busy until the UI task completes it (the answer is then discarded).

enum class GlinkMailboxState : uint8_t { IDLE, QUEUED, TAKEN, DONE };

// What the UI task answers. START: accepted, the lead-in has begun (stop never goes through the mailbox).
enum class GlinkReplyKind : uint8_t { FAILURE, PROFILE_UPDATE, SELECT, START };

struct GlinkMailboxResult {
    GlinkReplyKind kind = GlinkReplyKind::FAILURE;
    GlinkFailure failure;       // FAILURE
    GlinkProfileEntry entry;    // PROFILE_UPDATE: the stored profile after the write
    bool changed = false;       // PROFILE_UPDATE
    bool stored = false;        // PROFILE_UPDATE
    uint8_t select_profile = 0; // SELECT
    uint32_t lead_in_ms = 0;    // START
};

struct GlinkMailbox {
    GlinkMailboxState state = GlinkMailboxState::IDLE;
    bool abandoned = false;     // the Bluetooth task gave up on a TAKEN request
    uint32_t queued_at_ms = 0;
    uint32_t taken_at_ms = 0;
    GlinkRequest request;       // QUEUED, TAKEN
    GlinkMailboxResult result;  // DONE
};

// Bluetooth task: false unless the mailbox is IDLE (the request is then not filed)
bool glink_mailbox_submit(GlinkMailbox* box, const GlinkRequest& request, uint32_t now_ms);

enum class GlinkMailboxWait : uint8_t {
    WAITING,  // poll again on the next tick
    DONE,     // *out holds the answer and the mailbox is IDLE again
    TIMEOUT   // answer UI_BUSY: a QUEUED request was withdrawn, a TAKEN one is abandoned
};
// An IDLE mailbox (nothing was filed) reports TIMEOUT so a caller can never wait for nothing.
GlinkMailboxWait glink_mailbox_poll(GlinkMailbox* box, uint32_t now_ms, uint32_t pickup_timeout_ms,
                                    uint32_t run_timeout_ms, GlinkMailboxResult* out);

// UI task: QUEUED -> TAKEN and copies the request. False when nothing is queued or the pickup deadline has passed
// (the request is then for the poll to withdraw).
bool glink_mailbox_take(GlinkMailbox* box, GlinkRequest* out, uint32_t now_ms, uint32_t pickup_timeout_ms);
// UI task: TAKEN -> DONE (straight to IDLE when abandoned). Ignored in any other state.
void glink_mailbox_complete(GlinkMailbox* box, const GlinkMailboxResult& result);

inline bool glink_mailbox_busy(const GlinkMailbox& box) { return box.state != GlinkMailboxState::IDLE; }

// A snapshot published at published_ms is still good at now_ms (never published = not fresh; wrap-safe)
bool glink_snapshot_fresh(bool published, uint32_t published_ms, uint32_t now_ms, uint32_t stale_ms);
