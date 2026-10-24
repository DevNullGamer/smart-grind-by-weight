#include "pull_ui_text.h"
#include <stdio.h>
#include <string.h>
#include "update_prompt_text.h"

namespace {

bool starts_with_digit(const char* text) {
    return text && text[0] >= '0' && text[0] <= '9';
}

// "v1.5.0" whether or not the source already has the "v"
const char* with_v(const char* version) {
    return starts_with_digit(version) ? "v" : "";
}

void copy_text(char* out, size_t out_len, const char* text) {
    snprintf(out, out_len, "%s", text);
}

void set_notice(PullPageView* out, const char* format, const char* detail) {
    snprintf(out->notice, sizeof(out->notice), format, detail);
}

void fill_latest(const PullPageInput& in, bool stale, PullPageView* out) {
    out->latest_tone = PullTone::NORMAL;
    switch (in.state) {
        case PullState::IDLE: copy_text(out->latest, sizeof(out->latest), "--"); break;
        case PullState::CHECKING: copy_text(out->latest, sizeof(out->latest), "Checking..."); break;
        case PullState::UP_TO_DATE:
            copy_text(out->latest, sizeof(out->latest), in.no_release ? "No release published" : "Up to date");
            break;
        case PullState::AVAILABLE:
            if (stale || !in.offer_version || in.offer_version[0] == '\0') {
                copy_text(out->latest, sizeof(out->latest), "Check again");
            } else {
                snprintf(out->latest, sizeof(out->latest), "%s%s", with_v(in.offer_version), in.offer_version);
                out->latest_tone = PullTone::GOOD;
            }
            break;
        case PullState::CHECK_FAILED:
            copy_text(out->latest, sizeof(out->latest), "Check failed");
            out->latest_tone = PullTone::WARN;
            break;
        case PullState::DOWNLOADING:
        case PullState::VERIFYING:
        case PullState::INSTALLED: copy_text(out->latest, sizeof(out->latest), "Installing..."); break;
        case PullState::FAILED:
            copy_text(out->latest, sizeof(out->latest), "Install failed");
            out->latest_tone = PullTone::WARN;
            break;
    }
}

void fill_notice(const PullPageInput& in, bool stale, PullPageView* out) {
    out->notice[0] = '\0';
    if (in.notice != PullFailure::NONE) {
        set_notice(out, "%s.", pull_failure_text(in.notice));
        return;
    }
    switch (in.state) {
        case PullState::FAILED:
            set_notice(out, "Install failed: %s.", pull_failure_text(in.failure));
            if (!in.ble_enabled) {
                // The install turned Bluetooth off; the user has to turn it on again
                const size_t used = strlen(out->notice);
                snprintf(out->notice + used, sizeof(out->notice) - used, " Bluetooth is off, turn it on in the Bluetooth page.");
            }
            return;
        case PullState::CHECK_FAILED:
            if (in.failure == PullFailure::NO_CONNECTION) {
                copy_text(out->notice, sizeof(out->notice), "Could not reach the update server.");
            } else {
                set_notice(out, "Check failed: %s.", pull_failure_text(in.failure));
            }
            return;
        case PullState::AVAILABLE:
            if (stale && in.wifi_connected) {
                copy_text(out->notice, sizeof(out->notice), "This result is old. Check again.");
                return;
            }
            break;
        default: break;
    }
    if (!in.wifi_connected && !pull_state_is_running(in.state)) {
        copy_text(out->notice, sizeof(out->notice), "Connect to WiFi to check for updates.");
    }
}

}  // namespace

const char* pull_installed_text(const char* version, int build, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    if (!version || version[0] == '\0') {
        copy_text(out, out_len, "--");
    } else if (build < 0) {
        snprintf(out, out_len, "%s%s", with_v(version), version);
    } else {
        snprintf(out, out_len, "%s%s (#%d)", with_v(version), version, build);
    }
    return out;
}

bool pull_result_is_stale(uint32_t result_ms, uint32_t now_ms, bool wifi_connected) {
    if (!wifi_connected) return true;
    return static_cast<uint32_t>(now_ms - result_ms) > UPDATE_PULL_RESULT_STALE_MS;
}

void pull_page_view(const PullPageInput& in, PullPageView* out) {
    if (!out) return;
    const bool stale = pull_result_is_stale(in.result_ms, in.now_ms, in.wifi_connected);
    const bool can_check = pull_can_start_check(in.state);

    fill_latest(in, stale, out);
    fill_notice(in, stale, out);

    copy_text(out->check_label, sizeof(out->check_label), in.state == PullState::CHECKING ? "Checking..." : "Check for update");
    out->check_enabled = in.wifi_connected && can_check;
    out->beta_enabled = can_check;

    const bool offer_known = in.offer_version && in.offer_version[0] != '\0';
    out->show_install = in.state == PullState::AVAILABLE && offer_known && !stale;
    if (out->show_install) {
        snprintf(out->install_label, sizeof(out->install_label), "Install %s%s", with_v(in.offer_version), in.offer_version);
    } else {
        out->install_label[0] = '\0';
    }
}

const char* pull_confirm_message(const char* offer_version, const char* installed_version, uint32_t size, char* out,
                                 size_t out_len) {
    if (!out || out_len == 0) return out;
    const char* offer = offer_version ? offer_version : "";
    const char* installed = installed_version ? installed_version : "";
    char size_text[12];
    prompt_format_size(size, size_text, sizeof(size_text));
    snprintf(out, out_len,
             "Install %s%s?\nNow: %s%s\nSize: %s\n\nBluetooth is turned off meanwhile. The grinder restarts when done.",
             with_v(offer), offer, with_v(installed), installed, size_text);
    return out;
}
