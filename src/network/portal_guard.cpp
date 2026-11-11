#include "network/portal_guard.h"
#include <string.h>

//------------------------------------------------------------------------------
// Gate
//------------------------------------------------------------------------------
bool portal_route_enabled(PortalRoute route, bool web_enabled, bool push_enabled) {
    switch (route) {
        case PortalRoute::INFO: return true;
        case PortalRoute::UPDATE_PUSH: return push_enabled;
        case PortalRoute::PAGE:
        case PortalRoute::API_READ:
        case PortalRoute::API_WRITE:
        case PortalRoute::API_COMMAND: return web_enabled;
    }
    return false;
}

PortalError portal_route_refusal(PortalRoute route) {
    switch (route) {
        case PortalRoute::PAGE:
        case PortalRoute::API_READ:
        case PortalRoute::API_WRITE:
        case PortalRoute::API_COMMAND: return PortalError::WEB_INTERFACE_OFF;
        default: return PortalError::NONE;
    }
}

bool portal_server_should_run(bool wifi_connected, bool web_enabled, bool push_enabled) {
    return wifi_connected && (web_enabled || push_enabled);
}

//------------------------------------------------------------------------------
// Host / Origin / Content-Type / custom header
//------------------------------------------------------------------------------
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (lower(c) >= 'a' && lower(c) <= 'f'); }

// 1-5 digits, value 1..65535
bool port_valid(const char* s, size_t n) {
    if (n == 0 || n > 5) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) return false;
        v = v * 10 + static_cast<uint32_t>(s[i] - '0');
    }
    return v >= 1 && v <= 65535;
}

// Dotted quad, each part 0..255 in 1-3 digits without leading zeros
bool ipv4_valid(const char* s, size_t n) {
    int parts = 0;
    size_t i = 0;
    while (true) {
        const size_t start = i;
        uint32_t v = 0;
        while (i < n && is_digit(s[i])) {
            v = v * 10 + static_cast<uint32_t>(s[i] - '0');
            i++;
            if (i - start > 3) return false;
        }
        const size_t digits = i - start;
        if (digits == 0 || v > 255 || (digits > 1 && s[start] == '0')) return false;
        parts++;
        if (i == n) break;
        if (s[i] != '.') return false;
        i++;
    }
    return parts == 4;
}

// Hex groups of 1-4 digits separated by ':', at most one "::", optionally ending in an IPv4 literal
bool ipv6_valid(const char* s, size_t n) {
    if (n < 2 || n > 45) return false;
    int groups = 0;
    bool compressed = false;
    size_t i = 0;
    if (s[0] == ':') {
        if (s[1] != ':') return false;
        compressed = true;
        i = 2;
        if (i == n) return true;  // "::"
    }
    while (i < n) {
        const size_t start = i;
        bool dotted = false;
        while (i < n && s[i] != ':') {
            dotted = dotted || s[i] == '.';
            i++;
        }
        const size_t len = i - start;
        if (len == 0) return false;
        if (dotted) {
            if (i != n || !ipv4_valid(s + start, len)) return false;
            groups += 2;
        } else {
            if (len > 4) return false;
            for (size_t k = start; k < i; k++) {
                if (!is_hex(s[k])) return false;
            }
            groups++;
        }
        if (i == n) break;
        i++;  // ':'
        if (i < n && s[i] == ':') {
            if (compressed) return false;
            compressed = true;
            i++;
        } else if (i == n) {
            return false;  // single trailing ':'
        }
    }
    return compressed ? groups <= 7 : groups == 8;
}

// "<label>(.<label>)*.local": labels 1-63 of [a-z0-9-] (any case), not starting/ending with '-', at least two
bool local_name_valid(const char* s, size_t n) {
    if (n == 0 || n > 253) return false;
    size_t labels = 0;
    size_t i = 0;
    size_t last_start = 0;
    while (i < n) {
        const size_t start = i;
        while (i < n && s[i] != '.') {
            const char c = lower(s[i]);
            if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '-')) return false;
            i++;
        }
        const size_t len = i - start;
        if (len == 0 || len > 63 || s[start] == '-' || s[i - 1] == '-') return false;
        labels++;
        last_start = start;
        if (i < n) {
            i++;                       // '.'
            if (i == n) return false;  // trailing dot
        }
    }
    if (labels < 2 || n - last_start != 5) return false;
    const char* last = s + last_start;
    return lower(last[0]) == 'l' && lower(last[1]) == 'o' && lower(last[2]) == 'c' && lower(last[3]) == 'a' &&
           lower(last[4]) == 'l';
}

bool host_valid(const char* h, size_t n) {
    if (n == 0 || n > PORTAL_HEADER_MAX_LEN) return false;
    if (h[0] == '[') {
        const void* close = memchr(h, ']', n);
        if (!close) return false;
        const size_t end = static_cast<size_t>(static_cast<const char*>(close) - h);
        if (!ipv6_valid(h + 1, end - 1)) return false;
        if (end + 1 == n) return true;
        return h[end + 1] == ':' && port_valid(h + end + 2, n - end - 2);
    }
    size_t name_len = n;
    const void* colon = memchr(h, ':', n);
    if (colon) {
        name_len = static_cast<size_t>(static_cast<const char*>(colon) - h);
        if (!port_valid(h + name_len + 1, n - name_len - 1)) return false;
    }
    return ipv4_valid(h, name_len) || local_name_valid(h, name_len);
}

bool equal_nocase(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

}  // namespace

PortalError portal_check_host(const char* host) {
    if (!host) return PortalError::BAD_HOST;
    const size_t n = strnlen(host, PORTAL_HEADER_MAX_LEN + 1);
    return host_valid(host, n) ? PortalError::NONE : PortalError::BAD_HOST;
}

PortalError portal_check_origin(const char* origin, const char* host) {
    if (!origin) return PortalError::NONE;
    if (!host) return PortalError::BAD_ORIGIN;
    static const char kScheme[] = "http://";
    const size_t scheme_len = sizeof(kScheme) - 1;
    const size_t origin_len = strnlen(origin, PORTAL_HEADER_MAX_LEN + scheme_len + 1);
    const size_t host_len = strnlen(host, PORTAL_HEADER_MAX_LEN + 1);
    if (origin_len != scheme_len + host_len || memcmp(origin, kScheme, scheme_len) != 0) return PortalError::BAD_ORIGIN;
    return equal_nocase(origin + scheme_len, host, host_len) ? PortalError::NONE : PortalError::BAD_ORIGIN;
}

PortalError portal_check_content_type(const char* content_type) {
    if (!content_type) return PortalError::UNSUPPORTED_MEDIA_TYPE;
    const size_t n = strnlen(content_type, PORTAL_HEADER_MAX_LEN + 1);
    if (n > PORTAL_HEADER_MAX_LEN) return PortalError::UNSUPPORTED_MEDIA_TYPE;
    size_t end = 0;
    while (end < n && content_type[end] != ';') end++;
    size_t start = 0;
    while (start < end && (content_type[start] == ' ' || content_type[start] == '\t')) start++;
    while (end > start && (content_type[end - 1] == ' ' || content_type[end - 1] == '\t')) end--;
    static const char kJson[] = "application/json";
    const size_t json_len = sizeof(kJson) - 1;
    if (end - start != json_len || !equal_nocase(content_type + start, kJson, json_len)) {
        return PortalError::UNSUPPORTED_MEDIA_TYPE;
    }
    return PortalError::NONE;
}

PortalError portal_check_custom_header(const char* value) {
    if (!value || strcmp(value, PORTAL_CUSTOM_HEADER_VALUE) != 0) return PortalError::MISSING_CUSTOM_HEADER;
    return PortalError::NONE;
}

PortalError portal_check_request(const PortalRequestHeaders& h, bool state_changing) {
    PortalError err = portal_check_host(h.host);
    if (err != PortalError::NONE) return err;
    err = portal_check_origin(h.origin, h.host);
    if (err != PortalError::NONE) return err;
    if (!state_changing) return PortalError::NONE;
    err = portal_check_content_type(h.content_type);
    if (err != PortalError::NONE) return err;
    return portal_check_custom_header(h.custom);
}

//------------------------------------------------------------------------------
// Decisions
//------------------------------------------------------------------------------
PortalError portal_error_from_start_block(GrindStartBlock block) {
    switch (block) {
        case GrindStartBlock::NONE: return PortalError::NONE;
        case GrindStartBlock::MOTOR_DISABLED: return PortalError::MOTOR_DISABLED;
        case GrindStartBlock::UPDATE_IN_PROGRESS: return PortalError::UPDATE_IN_PROGRESS;
        case GrindStartBlock::LOAD_CELL_NOT_CONNECTED: return PortalError::LOAD_CELL_NOT_CONNECTED;
        case GrindStartBlock::LOAD_CELL_NO_DATA: return PortalError::LOAD_CELL_NO_DATA;
        case GrindStartBlock::LOAD_CELL_RATE_INVALID: return PortalError::LOAD_CELL_RATE_INVALID;
        case GrindStartBlock::MOTOR_FAULT: return PortalError::MOTOR_FAULT;  // same code as the motor test
    }
    return PortalError::BUSY;  // unknown block: refuse
}

PortalError portal_error_from_motor_test_block(MotorTestBlock block) {
    switch (block) {
        case MotorTestBlock::NONE: return PortalError::NONE;
        case MotorTestBlock::MOTOR_DISABLED: return PortalError::MOTOR_DISABLED;
        case MotorTestBlock::UPDATE_IN_PROGRESS: return PortalError::UPDATE_IN_PROGRESS;
        case MotorTestBlock::BUSY: return PortalError::BUSY;
        case MotorTestBlock::MOTOR_FAULT: return PortalError::MOTOR_FAULT;
        case MotorTestBlock::START_FAILED: return PortalError::BUSY;  // produced by begin_motor_test(), not by motor_test_block()
    }
    return PortalError::BUSY;
}

PortalError portal_command_decide(PortalCommand cmd, const PortalDecisionInput& in) {
    if (cmd == PortalCommand::STOP) return PortalError::NONE;
    if (in.command_in_flight) return PortalError::COMMAND_IN_FLIGHT;
    if (in.update_held) return PortalError::UPDATE_IN_PROGRESS;
    if (in.busy) return PortalError::BUSY;
    switch (cmd) {
        case PortalCommand::TARE:
            return in.ui == PortalUiMode::READY ? PortalError::NONE : PortalError::NOT_READY;
        case PortalCommand::START:
            if (in.ui != PortalUiMode::READY) return PortalError::NOT_READY;
            return portal_error_from_start_block(in.start_block);
        case PortalCommand::MOTOR_TEST:
            if (in.ui == PortalUiMode::OTHER) return PortalError::NOT_READY;
            return portal_error_from_motor_test_block(in.motor_test_block);
        case PortalCommand::STOP: break;
    }
    return PortalError::NONE;
}

PortalError portal_write_decide(const PortalDecisionInput& in) {
    if (in.command_in_flight) return PortalError::COMMAND_IN_FLIGHT;
    if (in.update_held) return PortalError::UPDATE_IN_PROGRESS;  // an update owns the flash: no NVS writes now
    if (in.busy) return PortalError::BUSY;
    return in.ui == PortalUiMode::OTHER ? PortalError::NOT_READY : PortalError::NONE;
}

//------------------------------------------------------------------------------
// Lead-in
//------------------------------------------------------------------------------
void portal_lead_in_begin(PortalLeadIn* lead_in, uint32_t now_ms) {
    lead_in->active = true;
    lead_in->cancelled = false;
    lead_in->start_ms = now_ms;
}

void portal_lead_in_cancel(PortalLeadIn* lead_in) {
    if (lead_in->active) lead_in->cancelled = true;
}

PortalLeadInState portal_lead_in_poll(PortalLeadIn* lead_in, uint32_t now_ms) {
    if (!lead_in->active) return PortalLeadInState::IDLE;
    if (lead_in->cancelled) {
        *lead_in = PortalLeadIn();
        return PortalLeadInState::CANCELLED;
    }
    if (static_cast<uint32_t>(now_ms - lead_in->start_ms) >= PORTAL_LEAD_IN_MS) {
        *lead_in = PortalLeadIn();
        return PortalLeadInState::GO;
    }
    return PortalLeadInState::WAITING;
}

bool portal_lead_in_touched(const PortalLeadIn& lead_in, uint32_t now_ms, uint32_t ms_since_last_touch) {
    if (!lead_in.active) return false;
    return ms_since_last_touch < static_cast<uint32_t>(now_ms - lead_in.start_ms);
}

uint32_t portal_lead_in_remaining_s(const PortalLeadIn& lead_in, uint32_t now_ms) {
    if (!lead_in.active) return 0;
    const uint32_t elapsed = static_cast<uint32_t>(now_ms - lead_in.start_ms);
    if (elapsed >= PORTAL_LEAD_IN_MS) return 0;
    return (PORTAL_LEAD_IN_MS - elapsed + 999) / 1000;
}
