#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pull_logic.h"
#include "release_index.h"

// Pure texts and decisions of the "Firmware" section in the WiFi menu page (no Arduino/ESP/LVGL dependencies, unit
// tested on the host). The LVGL side lives in ui/screens/menu_screen_wifi.cpp, the engine in network/update_pull.*.

#define PULL_UI_VERSION_BUF (SEMVER_PRE_MAX + 4)   // "v" + version + NUL, with room to spare
#define PULL_UI_LABEL_BUF 24                       // button labels ("Check for update")
#define PULL_UI_INSTALL_LABEL_BUF (SEMVER_PRE_MAX + 12)  // "Install v<version>"
#define PULL_UI_NOTICE_BUF 128                     // hint / error line under the buttons
#define PULL_UI_CONFIRM_BUF 256                    // message of the Install confirm dialog

// "v1.4.1-rc.3 (#217)". A "v" is added when the version starts with a digit; "(#N)" is left out when build < 0.
// Null / empty version gives "--". Returns out.
const char* pull_installed_text(const char* version, int build, char* out, size_t out_len);

// True when an AVAILABLE result may no longer be trusted: WiFi is gone (the network or the grinder moved) or the
// result is older than UPDATE_PULL_RESULT_STALE_MS (wrap-safe unsigned difference).
bool pull_result_is_stale(uint32_t result_ms, uint32_t now_ms, bool wifi_connected);

enum class PullTone : uint8_t { NORMAL, GOOD, WARN };

struct PullPageInput {
    PullState state = PullState::IDLE;
    PullFailure failure = PullFailure::NONE;   // why CHECK_FAILED / FAILED happened
    PullFailure notice = PullFailure::NONE;    // refusal of the user's last tap (shown first)
    bool no_release = false;                   // UP_TO_DATE because the channel is empty
    bool wifi_connected = false;
    bool ble_enabled = true;
    const char* offer_version = nullptr;       // latest release known from the last check (null: none)
    uint32_t result_ms = 0;                    // UpdatePull::result_at_ms()
    uint32_t now_ms = 0;
};

struct PullPageView {
    char latest[PULL_UI_VERSION_BUF + 8];      // value of the "Latest:" row
    PullTone latest_tone = PullTone::NORMAL;
    char check_label[PULL_UI_LABEL_BUF];
    bool check_enabled = false;
    bool beta_enabled = false;                 // the channel may be changed (no job is running)
    bool show_install = false;
    char install_label[PULL_UI_INSTALL_LABEL_BUF];
    char notice[PULL_UI_NOTICE_BUF];           // "" when there is nothing to say
};

// Everything the section shows for one pull state. Install is offered only in AVAILABLE with a known version, WiFi up
// and a fresh result. Always fills every field of *out (strings NUL-terminated).
void pull_page_view(const PullPageInput& in, PullPageView* out);

// Message of the Install confirm dialog (lines separated by '\n'):
//   Install v1.5.0?
//   Now: v1.4.1-rc.3
//   Size: 2.2 MB
//   <blank>
//   Bluetooth is turned off meanwhile. The grinder restarts when done.
// Needs PULL_UI_CONFIRM_BUF bytes; never writes more than out_len. Returns out.
const char* pull_confirm_message(const char* offer_version, const char* installed_version, uint32_t size, char* out,
                                 size_t out_len);
