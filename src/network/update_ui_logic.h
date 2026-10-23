#pragma once
#include <stddef.h>
#include <stdint.h>
#include "update_status.h"
#include "../config/update.h"

// Pure decisions and texts of the WiFi update UI (no Arduino/ESP/LVGL dependencies, unit tested on the host).
// The LVGL side lives in ui/controllers/wifi_update_controller.*.

enum class PromptAction : uint8_t {
    NONE,         // nothing to do this frame
    SHOW,         // open the Accept dialog for the pending request
    REFUSE_BUSY,  // the screen cannot show it: answer the request with UI_BUSY
    DISMISS       // the dialog is up but its request is gone (expired, cancelled, replaced): close it
};

// pending: a request waits for an answer. dialog_shown: our dialog is up. same_request: the pending request is the
// one the dialog was opened for. ui_allows: the screen is on READY or MENU (see update_ui_state_allows_prompt).
PromptAction update_prompt_action(bool pending, bool dialog_shown, bool same_request, bool ui_allows);

// The prompt may only open on the home screen or the menu; every other state (grinding, editing, calibration,
// another dialog, update screens, ...) could lose unsaved work or clobber the single dialog slot.
bool update_ui_state_allows_prompt(bool state_is_ready, bool state_is_menu);

// Arc value 0-100 for received/total bytes (0 when total is 0; capped at 100)
int update_progress_percent(uint32_t received, uint32_t total);

// Status line of the progress screen: "Receiving update...", "Verifying...", "Installed. Restarting...",
// "Failed: <reason>" (reason shortened to UPDATE_UI_REASON_MAX characters, printable ASCII only; "Update failed"
// when empty). Other phases give "". Needs UPDATE_UI_STATUS_BUF bytes. Returns out.
const char* update_progress_status_text(UpdatePhase phase, const char* reason, char* out, size_t out_len);

// "Listening:\ngrindbyweight.local" (two rows so it fits 280 px; host is cut if absurdly long). Needs 96 bytes. Returns out.
const char* update_listening_text(const char* host, char* out, size_t out_len);
