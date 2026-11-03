#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"
#include "../controllers/grind_start_check.h"
#include "../controllers/motor_test_check.h"
#include "portal_api.h"

// Pure guards and decisions of the web panel (docs/PORTAL-API.md), no Arduino/ESP/LVGL/NVS dependencies
// (unit tested on the host). The panel has no token: the on-screen "Web interface" switch is the gate, these
// checks only keep a browser from being turned against the grinder.
//
// Order the HTTP layer applies them: route gate -> browser guards -> body parser -> decision (UI task).

//------------------------------------------------------------------------------
// Web interface gate (own switch, independent of Receive = firmware push)
//------------------------------------------------------------------------------
enum class PortalRoute : uint8_t {
    INFO,         // GET /api/info (open, unchanged, never gated by the web switch)
    PAGE,         // GET /
    API_READ,     // GET /api/status, /api/settings, /api/schema
    API_WRITE,    // PATCH /api/settings
    API_COMMAND,  // POST /api/command (including stop)
    UPDATE_PUSH   // /api/update/begin|data (firmware push, token protected)
};

// Each route checks only its own switch: the web routes need the web interface, the update routes need Receive
// (push_enabled), INFO is always open.
bool portal_route_enabled(PortalRoute route, bool web_enabled, bool push_enabled);
// The refusal of a disabled web route (always WEB_INTERFACE_OFF; NONE for INFO and UPDATE_PUSH, whose
// refusal belongs to the update code)
PortalError portal_route_refusal(PortalRoute route);
// The HTTP server must exist while WiFi is connected and either switch is on
bool portal_server_should_run(bool wifi_connected, bool web_enabled, bool push_enabled);

//------------------------------------------------------------------------------
// Browser guards
//------------------------------------------------------------------------------
// Header values as received; nullptr = header absent.
struct PortalRequestHeaders {
    const char* host = nullptr;
    const char* origin = nullptr;
    const char* content_type = nullptr;
    const char* custom = nullptr;  // value of PORTAL_CUSTOM_HEADER_NAME
};

// Host: "<label>(.<label>)*.local" or an IPv4 / bracketed IPv6 literal, each with an optional ":port".
// Anything else (a DNS name, which could be a DNS-rebinding name, an empty or overlong value) is BAD_HOST.
PortalError portal_check_host(const char* host);
// Absent Origin is fine; a present one must be exactly "http://" + Host (case-insensitive), so "null" and
// foreign sites are BAD_ORIGIN. Call after portal_check_host.
PortalError portal_check_origin(const char* origin, const char* host);
// "application/json" (case-insensitive, parameters such as charset ignored)
PortalError portal_check_content_type(const char* content_type);
// PORTAL_CUSTOM_HEADER_VALUE exactly
PortalError portal_check_custom_header(const char* value);
// All guards in order: host, origin, then (state_changing only) content type and custom header.
PortalError portal_check_request(const PortalRequestHeaders& headers, bool state_changing);

//------------------------------------------------------------------------------
// Command and write decision (inputs are gathered by the UI task right before acting)
//------------------------------------------------------------------------------
enum class PortalUiMode : uint8_t {
    READY,  // the READY screen, no dialog or overlay
    MENU,   // a menu page
    OTHER   // grinding screens, edit, calibration, confirm and update dialogs, ...
};

struct PortalDecisionInput {
    PortalUiMode ui = PortalUiMode::OTHER;
    bool busy = false;               // grind, autotune or motor output active
    bool update_held = false;        // UpdateGuard::is_held()
    bool command_in_flight = false;  // another write/command (not stop) is still in the mailbox
    GrindStartBlock start_block = GrindStartBlock::NONE;       // GrindController::start_block()
    MotorTestBlock motor_test_block = MotorTestBlock::NONE;    // UIManager::motor_pulse_block()
};

// NONE when the command may run, else the refusal. STOP is always accepted. Otherwise, in this order:
// command in flight, update held, busy, UI mode (tare/start need READY, motor test READY or MENU), then the
// command's own block (start: start_block, motor test: motor_test_block).
PortalError portal_command_decide(PortalCommand cmd, const PortalDecisionInput& in);
// Settings/profile writes: command in flight, update held (it owns the flash), busy, then READY or MENU only
// (never over an EDIT screen).
PortalError portal_write_decide(const PortalDecisionInput& in);
// Block -> refusal (NONE -> NONE)
PortalError portal_error_from_start_block(GrindStartBlock block);
PortalError portal_error_from_motor_test_block(MotorTestBlock block);

//------------------------------------------------------------------------------
// Touch-to-cancel lead-in after a remote start (PORTAL_LEAD_IN_MS)
//------------------------------------------------------------------------------
enum class PortalLeadInState : uint8_t {
    IDLE,       // no lead-in running
    WAITING,    // counting down; the screen shows "Remote start in N s, touch to cancel"
    CANCELLED,  // a touch or stop ended it (reported once)
    GO          // the time is up: start the grind now (reported once)
};

struct PortalLeadIn {
    bool active = false;
    bool cancelled = false;
    uint32_t start_ms = 0;
};

void portal_lead_in_begin(PortalLeadIn* lead_in, uint32_t now_ms);
// A touch on the grinder or a stop request; ignored while no lead-in runs
void portal_lead_in_cancel(PortalLeadIn* lead_in);
// Advances the lead-in (wrap-safe for millis()). CANCELLED wins over GO when both are due.
PortalLeadInState portal_lead_in_poll(PortalLeadIn* lead_in, uint32_t now_ms);
// True when the touch screen was touched after the lead-in began (ms_since_last_touch is the touch driver's age
// of its last touch, so a touch strictly newer than the start counts); false while no lead-in runs
bool portal_lead_in_touched(const PortalLeadIn& lead_in, uint32_t now_ms, uint32_t ms_since_last_touch);
// Whole seconds left for the screen text, rounded up (0 when idle or elapsed)
uint32_t portal_lead_in_remaining_s(const PortalLeadIn& lead_in, uint32_t now_ms);
