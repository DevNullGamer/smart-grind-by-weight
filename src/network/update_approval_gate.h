#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include "update_approval.h"
#include "../config/update.h"

// What the prompt shows (already sanitised by prompt_build_message when displayed)
struct ApprovalPrompt {
    uint32_t size = 0;
    int32_t build = -1;                              // -1: absent
    char version[UPDATE_PROMPT_TEXT_MAX + 1] = {0};  // empty: absent
    char peer_ip[UPDATE_PEER_IP_BUF] = {0};          // empty: unknown
};

/**
 * UpdateApprovalGate - thread-safe wrapper around the pure UpdateApproval state machine.
 *
 * The HTTP task files a request (request()) and waits (wait()); the UI task polls pending_prompt() and answers
 * with resolve()/resolve_busy(). The network layer never touches LVGL: the UI only reads this gate. All
 * critical sections are a few word copies (spinlock, no blocking call inside).
 *
 * Until a UI controller calls attach_ui(true) nobody can answer a prompt, so the policy refuses with
 * UI_BUSY instead of waiting (fail closed).
 */
class UpdateApprovalGate {
public:
    ApprovalRequestResult request(const ApprovalPrompt& prompt, uint32_t timeout_ms, uint32_t backoff_ms);
    // Blocks the calling (HTTP) task until the request is no longer PENDING (the timeout expires it), then
    // returns the final state. Does not clear it.
    ApprovalState wait();
    ApprovalState poll();
    void clear();

    // UI side
    // True while PENDING (copy). generation (optional) is read under the same lock, so it always belongs to *out.
    bool pending_prompt(ApprovalPrompt* out, uint32_t* generation = nullptr);
    bool resolve(bool accept);
    // Only answers the request with this generation (false if it was replaced meanwhile): binds a tap to the request it was shown for
    bool resolve(bool accept, uint32_t generation);
    bool resolve_busy();
    // Withdraws a pending request; true if there was one
    bool cancel();

    // True from filing a request until the HTTP task has cleared the final state: the server must not stop now
    bool in_use();
    uint32_t backoff_remaining_ms();
    uint32_t remaining_ms();         // until the pending request expires (0 if none)
    // Incremented for every filed request; lets the UI tell a new prompt from the one it already shows
    uint32_t generation() const { return generation_; }

    void attach_ui(bool attached) { ui_attached_ = attached; }
    bool ui_attached() const { return ui_attached_; }

private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    UpdateApproval approval_;
    ApprovalPrompt prompt_;
    volatile uint32_t generation_ = 0;
    volatile bool ui_attached_ = false;
};

extern UpdateApprovalGate update_approval;
