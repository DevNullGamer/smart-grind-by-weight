#include "update_ui_logic.h"
#include <stdio.h>
#include <string.h>

PromptAction update_prompt_action(bool pending, bool dialog_shown, bool same_request, bool ui_allows) {
    if (dialog_shown) {
        // The request this dialog belongs to is over (or replaced by a newer one): close the dialog first
        return (!pending || !same_request) ? PromptAction::DISMISS : PromptAction::NONE;
    }
    if (!pending) return PromptAction::NONE;
    return ui_allows ? PromptAction::SHOW : PromptAction::REFUSE_BUSY;
}

bool update_ui_state_allows_prompt(bool state_is_ready, bool state_is_menu) {
    return state_is_ready || state_is_menu;
}

int update_progress_percent(uint32_t received, uint32_t total) {
    if (total == 0) return 0;
    if (received >= total) return 100;
    return static_cast<int>((static_cast<uint64_t>(received) * 100u) / total);
}

const char* update_progress_status_text(UpdatePhase phase, const char* reason, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    out[0] = '\0';
    switch (phase) {
        case UpdatePhase::RECEIVING: snprintf(out, out_len, "Receiving update..."); break;
        case UpdatePhase::VERIFYING: snprintf(out, out_len, "Verifying..."); break;
        case UpdatePhase::INSTALLED: snprintf(out, out_len, "Installed. Restarting..."); break;
        case UpdatePhase::FAILED: {
            char shown[UPDATE_UI_REASON_MAX + 1];
            size_t n = 0;
            const size_t len = reason ? strlen(reason) : 0;
            const bool too_long = len > UPDATE_UI_REASON_MAX;
            const size_t keep = too_long ? UPDATE_UI_REASON_MAX - 3 : len;
            for (; n < keep; n++) {
                const unsigned char c = static_cast<unsigned char>(reason[n]);
                shown[n] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
            }
            if (too_long) {
                shown[n++] = '.';
                shown[n++] = '.';
                shown[n++] = '.';
            }
            shown[n] = '\0';
            if (n == 0) {
                snprintf(out, out_len, "Update failed");
            } else {
                snprintf(out, out_len, "Failed: %s", shown);
            }
            break;
        }
        default: break;
    }
    return out;
}

const char* update_listening_text(const char* host, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    snprintf(out, out_len, "Listening:\n%.63s.local", host ? host : "");
    return out;
}
