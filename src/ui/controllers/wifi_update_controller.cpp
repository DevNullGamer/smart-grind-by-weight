#include "wifi_update_controller.h"

#include <Arduino.h>
#include <cstring>

#include "../../config/constants.h"
#include "../../network/update_approval_gate.h"
#include "../../network/update_pull.h"
#include "../../network/update_prompt_text.h"
#include "../../network/update_status.h"
#include "../../network/update_ui_logic.h"
#include "../ui_manager.h"

WifiUpdateUIController::WifiUpdateUIController(UIManager* manager)
    : ui_manager_(manager), return_state_(UIState::READY) {
    static_assert(sizeof(last_status_) >= UPDATE_UI_STATUS_BUF, "status cache too small");
}

WifiUpdateUIController::~WifiUpdateUIController() {
    // Nobody can answer prompts any more: the policy fails closed again
    update_approval.attach_ui(false);
}

bool WifiUpdateUIController::update() {
    if (!ui_manager_ || !ui_manager_->state_machine) {
        return false;
    }

    // First frame: the UI exists and is polled, so prompts can be answered from here on
    if (!ui_attached_) {
        ui_attached_ = true;
        update_approval.attach_ui(true);
    }

    update_prompt();
    const bool consumed = update_progress();
    sync_hold();
    return consumed;
}

//------------------------------------------------------------------------------
// Accept dialog
//------------------------------------------------------------------------------
void WifiUpdateUIController::update_prompt() {
    auto* state_machine = ui_manager_->state_machine;

    ApprovalPrompt prompt;
    uint32_t generation = 0;
    const bool pending = update_approval.pending_prompt(&prompt, &generation);
    prompt_pending_ = pending;

    // Our dialog was replaced by another screen (e.g. a Bluetooth update took over): withdraw the request
    if (dialog_shown_ && !state_machine->is_state(UIState::CONFIRM)) {
        dialog_shown_ = false;
        update_approval.cancel();
        LOG_BLE("[UI] WiFi update dialog lost its screen, request withdrawn\n");
        return;
    }

    const bool ui_allows =
        update_ui_state_allows_prompt(state_machine->is_state(UIState::READY), state_machine->is_state(UIState::MENU)) &&
        !UpdateStatus::active() && !BlockingOperationOverlay::getInstance().is_operation_active();

    switch (update_prompt_action(pending, dialog_shown_, generation == dialog_generation_, ui_allows)) {
        case PromptAction::SHOW: {
            char message[UPDATE_PROMPT_MESSAGE_BUF];
            prompt_build_message(prompt.size, prompt.build, prompt.version, prompt.peer_ip, message, sizeof(message));
            dialog_shown_ = true;
            dialog_generation_ = generation;
            LOG_BLE("[UI] WiFi update prompt shown\n");
            // The taps are bound to this request (generation) and ignored while a finger that was already on the
            // screen could still hit a button
            ui_manager_->show_confirmation("WIFI UPDATE", message, "INSTALL", lv_color_hex(THEME_COLOR_ACCENT),
                                           [this, generation]() { answer_prompt(true, generation); }, "DECLINE",
                                           [this, generation]() { answer_prompt(false, generation); });
            if (ui_manager_->confirm_controller_) ui_manager_->confirm_controller_->guard_taps(UPDATE_UI_TAP_GUARD_MS);
            break;
        }
        case PromptAction::REFUSE_BUSY:
            update_approval.resolve_busy();
            LOG_BLE("[UI] WiFi update prompt refused: screen busy\n");
            break;
        case PromptAction::DISMISS:
            dialog_shown_ = false;
            if (ui_manager_->confirm_controller_) ui_manager_->confirm_controller_->dismiss();
            LOG_BLE("[UI] WiFi update prompt dismissed (request over)\n");
            break;
        case PromptAction::NONE:
            break;
    }
}

// Runs inside the confirm dialog callback (the dialog closes itself afterwards)
void WifiUpdateUIController::answer_prompt(bool accept, uint32_t generation) {
    dialog_shown_ = false;
    const bool delivered = update_approval.resolve(accept, generation);
    LOG_BLE("[UI] WiFi update %s%s\n", accept ? "accepted" : "declined", delivered ? "" : " (too late, request was over)");
}

//------------------------------------------------------------------------------
// Progress screen
//------------------------------------------------------------------------------
// A pull job is "ours" from the install request to its end; a failure counts until the screen has shown it once
// A stale unacknowledged FAILED must not title a later push failure: with UpdateStatus content it only counts
// when that content came from the download source.
static bool is_pull_job(PullState pull, bool failure_acknowledged, bool status_content) {
    return pull == PullState::DOWNLOADING || pull == PullState::VERIFYING || pull == PullState::INSTALLED ||
           (pull == PullState::FAILED && !failure_acknowledged &&
            (!status_content || UpdateStatus::source() == UpdateSource::WIFI_DOWNLOAD));
}

bool WifiUpdateUIController::update_progress() {
    const UpdatePhase phase = UpdateStatus::phase();
    const PullState pull = update_pull.state();
    if (pull != PullState::FAILED) pull_failure_acked_ = false;  // a later failure is a new one

    const bool status_content = phase == UpdatePhase::RECEIVING || phase == UpdatePhase::VERIFYING ||
                                phase == UpdatePhase::INSTALLED || phase == UpdatePhase::FAILED;
    const PullProgressView pull_view = update_pull_progress_view(pull, phase, pull_failure_acked_);
    const bool has_content = status_content || pull_view != PullProgressView::NONE;

    if (!progress_shown_) {
        if (!has_content) return false;
        // A pull job that never reached UpdateStatus (dev builds start one without the menu) must not take over a
        // screen the user is working on
        auto* state_machine = ui_manager_->state_machine;
        const bool home = update_ui_state_allows_prompt(state_machine->is_state(UIState::READY),
                                                        state_machine->is_state(UIState::MENU));
        if (!status_content && !home) return false;
        begin_progress();
    } else if (!has_content) {
        // The status was reset under us (e.g. a new request while the failure was shown): leave the screen
        end_progress();
        return false;
    }

    // What to show: the UpdateStatus phase when it has content, else the pull job's own view
    char reason[48] = {0};
    char status[UPDATE_UI_STATUS_BUF];
    bool failed = false;
    int percent = 0;
    if (status_content) {
        failed = phase == UpdatePhase::FAILED;
        if (failed) UpdateStatus::reason(reason, sizeof(reason));
        update_progress_status_text(phase, reason, status, sizeof(status), pull_job_);
        if (phase == UpdatePhase::RECEIVING) {
            percent = update_progress_percent(UpdateStatus::received(), UpdateStatus::total());
        } else if (!failed) {
            percent = 100;
        }
    } else if (pull_view == PullProgressView::FAILED) {
        failed = true;
        update_progress_status_text(UpdatePhase::FAILED, pull_failure_text(update_pull.failure()), status, sizeof(status));
    } else {
        snprintf(status, sizeof(status), "%s", update_pull_connecting_text());
    }

    if (strcmp(status, last_status_) != 0) {
        ui_manager_->ota_screen.update_status(status);
        strncpy(last_status_, status, sizeof(last_status_) - 1);
        last_status_[sizeof(last_status_) - 1] = '\0';
    }

    if (!failed) {
        failed_since_ms_ = 0;  // a new transfer started while the screen stayed up: restart the hold next time
        if (percent != last_percent_) {
            ui_manager_->ota_screen.update_progress(percent);
            last_percent_ = percent;
        }
        return true;
    }

    // Failed: show the reason for a moment, then go back to where the user was
    const uint32_t now = millis();
    if (failed_since_ms_ == 0) {
        failed_since_ms_ = now ? now : 1;
    } else if (now - failed_since_ms_ >= UPDATE_UI_FAILED_HOLD_MS) {
        end_progress();
        return false;
    }
    return true;
}

void WifiUpdateUIController::begin_progress() {
    auto* state_machine = ui_manager_->state_machine;
    return_state_ = state_machine->is_state(UIState::MENU) ? UIState::MENU : UIState::READY;
    progress_shown_ = true;
    failed_since_ms_ = 0;
    last_percent_ = -1;
    last_status_[0] = '\0';
    const UpdatePhase phase = UpdateStatus::phase();
    const bool status_content = phase == UpdatePhase::RECEIVING || phase == UpdatePhase::VERIFYING ||
                                phase == UpdatePhase::INSTALLED || phase == UpdatePhase::FAILED;
    pull_job_ = is_pull_job(update_pull.state(), pull_failure_acked_, status_content);
    LOG_BLE("[UI] WiFi update progress screen shown (%s)\n", pull_job_ ? "download" : "push");

    ui_manager_->ota_screen.show_ota_mode();
    ui_manager_->ota_screen.update_title(update_progress_title(pull_job_));
    ui_manager_->switch_to_state(UIState::OTA_UPDATE);
}

void WifiUpdateUIController::end_progress() {
    UpdateStatus::acknowledge();  // a FAILED result must not persist
    // The pull job's FAILED state stays (the menu page explains it); the screen must not show it again
    if (update_pull.state() == PullState::FAILED) pull_failure_acked_ = true;
    progress_shown_ = false;
    failed_since_ms_ = 0;
    ui_manager_->ota_screen.hide();
    ui_manager_->switch_to_state(return_state_);
    LOG_BLE("[UI] WiFi update progress screen closed\n");
}

// Keeps the screen awake while a request waits, the dialog is up or progress is shown; released on every resolution
void WifiUpdateUIController::sync_hold() {
    const bool want = prompt_pending_ || dialog_shown_ || progress_shown_;
    if (want == hold_active_) return;
    hold_active_ = want;
    if (ui_manager_->screen_timeout_controller_) ui_manager_->screen_timeout_controller_->hold_awake(want);
}
