#include "update_approval_gate.h"

UpdateApprovalGate update_approval;

namespace {

// RAII spinlock section
struct Section {
    portMUX_TYPE* mux;
    explicit Section(portMUX_TYPE* m) : mux(m) { portENTER_CRITICAL(mux); }
    ~Section() { portEXIT_CRITICAL(mux); }
};

}  // namespace

ApprovalRequestResult UpdateApprovalGate::request(const ApprovalPrompt& prompt, uint32_t timeout_ms, uint32_t backoff_ms) {
    Section section(&mux_);
    const ApprovalRequestResult result = approval_request(&approval_, millis(), timeout_ms, backoff_ms);
    if (result == ApprovalRequestResult::OK) {
        prompt_ = prompt;
        generation_ = generation_ + 1;
    }
    return result;
}

ApprovalState UpdateApprovalGate::poll() {
    Section section(&mux_);
    return approval_poll(&approval_, millis());
}

ApprovalState UpdateApprovalGate::wait() {
    ApprovalState state = poll();
    while (state == ApprovalState::PENDING) {
        vTaskDelay(pdMS_TO_TICKS(UPDATE_PROMPT_POLL_MS));
        state = poll();
    }
    return state;
}

void UpdateApprovalGate::clear() {
    Section section(&mux_);
    approval_clear(&approval_);
}

bool UpdateApprovalGate::pending_prompt(ApprovalPrompt* out) {
    Section section(&mux_);
    if (approval_poll(&approval_, millis()) != ApprovalState::PENDING) return false;
    if (out) *out = prompt_;
    return true;
}

bool UpdateApprovalGate::resolve(bool accept) {
    Section section(&mux_);
    return approval_resolve(&approval_, accept, millis());
}

bool UpdateApprovalGate::resolve_busy() {
    Section section(&mux_);
    return approval_resolve_busy(&approval_);
}

bool UpdateApprovalGate::cancel() {
    Section section(&mux_);
    return approval_cancel(&approval_);
}

bool UpdateApprovalGate::in_use() {
    Section section(&mux_);
    return approval_.state != ApprovalState::IDLE;
}

uint32_t UpdateApprovalGate::backoff_remaining_ms() {
    Section section(&mux_);
    return approval_backoff_remaining_ms(&approval_, millis());
}

uint32_t UpdateApprovalGate::remaining_ms() {
    Section section(&mux_);
    return approval_remaining_ms(&approval_, millis());
}
