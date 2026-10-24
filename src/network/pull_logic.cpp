#include "pull_logic.h"
#include <string.h>

const char* pull_failure_text(PullFailure failure) {
    switch (failure) {
        case PullFailure::NONE: return "";
        case PullFailure::BUSY: return "Already running";
        case PullFailure::NOT_AVAILABLE: return "Nothing to install";
        case PullFailure::NO_WIFI: return "No WiFi";
        case PullFailure::UPDATE_RUNNING: return "Another update running";
        case PullFailure::GRINDER_BUSY: return "Grinder busy";
        case PullFailure::BLE_CONNECTED: return "Disconnect Bluetooth first";
        case PullFailure::VERIFYING_FIRMWARE: return "Still verifying firmware";
        case PullFailure::LOW_MEMORY: return "Low memory";
        case PullFailure::NO_CONNECTION: return "No connection";
        case PullFailure::NOT_FOUND: return "Not found";
        case PullFailure::SERVER_ERROR: return "Server error";
        case PullFailure::BAD_REPLY: return "Bad server reply";
        case PullFailure::REPLY_TOO_LARGE: return "Reply too large";
        case PullFailure::INDEX_INVALID: return "Invalid release info";
        case PullFailure::DOWNLOAD_STALLED: return "Download stalled";
        case PullFailure::CONNECTION_LOST: return "Connection lost";
        case PullFailure::DOWNLOAD_INTERRUPTED: return "Download interrupted";
        case PullFailure::CHECKSUM_MISMATCH: return "Checksum mismatch";
        case PullFailure::NOT_FIRMWARE: return "Not a firmware image";
        case PullFailure::IMAGE_REJECTED: return "Image rejected";
        case PullFailure::FLASH_FAILED: return "Flash write failed";
        case PullFailure::INTERNAL: return "Update failed";
    }
    return "Update failed";
}

const char* pull_state_name(PullState state) {
    switch (state) {
        case PullState::IDLE: return "IDLE";
        case PullState::CHECKING: return "CHECKING";
        case PullState::UP_TO_DATE: return "UP_TO_DATE";
        case PullState::AVAILABLE: return "AVAILABLE";
        case PullState::CHECK_FAILED: return "CHECK_FAILED";
        case PullState::DOWNLOADING: return "DOWNLOADING";
        case PullState::VERIFYING: return "VERIFYING";
        case PullState::INSTALLED: return "INSTALLED";
        case PullState::FAILED: return "FAILED";
    }
    return "?";
}

bool pull_state_is_running(PullState state) {
    return state == PullState::CHECKING || state == PullState::DOWNLOADING || state == PullState::VERIFYING;
}

bool pull_can_start_check(PullState state) {
    switch (state) {
        case PullState::IDLE:
        case PullState::UP_TO_DATE:
        case PullState::AVAILABLE:
        case PullState::CHECK_FAILED:
        case PullState::FAILED: return true;
        default: return false;
    }
}

bool pull_can_start_install(PullState state) {
    return state == PullState::AVAILABLE;
}

bool pull_transition_allowed(PullState from, PullState to) {
    switch (from) {
        case PullState::IDLE: return to == PullState::CHECKING;
        case PullState::CHECKING:
            return to == PullState::UP_TO_DATE || to == PullState::AVAILABLE || to == PullState::CHECK_FAILED;
        case PullState::UP_TO_DATE:
        case PullState::CHECK_FAILED:
        case PullState::FAILED: return to == PullState::CHECKING || to == PullState::IDLE;
        case PullState::AVAILABLE: return to == PullState::CHECKING || to == PullState::DOWNLOADING || to == PullState::IDLE;
        case PullState::DOWNLOADING: return to == PullState::VERIFYING || to == PullState::FAILED;
        case PullState::VERIFYING: return to == PullState::INSTALLED || to == PullState::FAILED;
        case PullState::INSTALLED: return false;
    }
    return false;
}

PullFailure pull_refusal(PullState state, bool for_install, const PullPreconditions& facts) {
    if (for_install) {
        if (pull_state_is_running(state) || state == PullState::INSTALLED) return PullFailure::BUSY;
        if (!pull_can_start_install(state)) return PullFailure::NOT_AVAILABLE;
    } else if (!pull_can_start_check(state)) {
        return PullFailure::BUSY;
    }
    if (!facts.wifi_connected) return PullFailure::NO_WIFI;
    if (facts.update_active) return PullFailure::UPDATE_RUNNING;
    if (facts.grinder_busy) return PullFailure::GRINDER_BUSY;
    if (for_install) {
        if (facts.boot_pending) return PullFailure::VERIFYING_FIRMWARE;
        if (facts.ble_connected) return PullFailure::BLE_CONNECTED;
    }
    if (facts.internal_free < UPDATE_PULL_MIN_FREE_INTERNAL_BYTES) return PullFailure::LOW_MEMORY;
    return PullFailure::NONE;
}

PullIndexDecision pull_evaluate_index(ReleaseParseResult parsed, const ReleaseInfo* info, const char* installed_version) {
    PullIndexDecision decision;
    if (parsed == ReleaseParseResult::NONE) {
        decision.state = PullState::UP_TO_DATE;
        decision.no_release = true;
        return decision;
    }
    if (parsed != ReleaseParseResult::OK || !info) {
        decision.state = PullState::CHECK_FAILED;
        decision.failure = PullFailure::INDEX_INVALID;
        return decision;
    }
    decision.state = release_is_newer(installed_version, info->version) ? PullState::AVAILABLE : PullState::UP_TO_DATE;
    return decision;
}

bool pull_url_scheme_allowed(const char* url, bool allow_http) {
    if (!url) return false;
    if (strncmp(url, "https://", 8) == 0) return url[8] != '\0';
    return allow_http && strncmp(url, "http://", 7) == 0 && url[7] != '\0';
}

PullHttpCheck pull_check_response(int status, int64_t content_length, int64_t expected_length, int64_t max_length) {
    if (status != 200) return PullHttpCheck::BAD_STATUS;
    if (content_length <= 0) return PullHttpCheck::NO_LENGTH;  // absent, chunked (0 / -1) or an error code
    if (content_length > max_length) return PullHttpCheck::TOO_LARGE;
    if (expected_length >= 0 && content_length != expected_length) return PullHttpCheck::LENGTH_MISMATCH;
    return PullHttpCheck::OK;
}

PullFailure pull_failure_for_http_check(PullHttpCheck check, int status) {
    switch (check) {
        case PullHttpCheck::OK: return PullFailure::NONE;
        case PullHttpCheck::BAD_STATUS: return status == 404 ? PullFailure::NOT_FOUND : PullFailure::SERVER_ERROR;
        case PullHttpCheck::NO_LENGTH:
        case PullHttpCheck::LENGTH_MISMATCH: return PullFailure::BAD_REPLY;
        case PullHttpCheck::TOO_LARGE: return PullFailure::REPLY_TOO_LARGE;
    }
    return PullFailure::INTERNAL;
}

bool pull_download_deadline_exceeded(uint32_t started_ms, uint32_t now_ms, uint32_t max_ms) {
    return static_cast<uint32_t>(now_ms - started_ms) > max_ms;
}

namespace {
constexpr int kReadTimeoutCode = -0x7007;  // -ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE 0x7000 + 7)
}

PullReadOutcome pull_classify_read(int n, uint32_t received, uint32_t total) {
    if (n > 0) {
        return static_cast<uint64_t>(received) + static_cast<uint32_t>(n) > total ? PullReadOutcome::OVERRUN
                                                                                    : PullReadOutcome::DATA;
    }
    if (n == 0) return received == total ? PullReadOutcome::DONE : PullReadOutcome::TRUNCATED;
    return n == kReadTimeoutCode ? PullReadOutcome::TIMEOUT : PullReadOutcome::READ_ERROR;
}

PullFailure pull_failure_for_read(PullReadOutcome outcome) {
    switch (outcome) {
        case PullReadOutcome::DATA:
        case PullReadOutcome::DONE: return PullFailure::NONE;
        case PullReadOutcome::TRUNCATED: return PullFailure::DOWNLOAD_INTERRUPTED;
        case PullReadOutcome::TIMEOUT: return PullFailure::DOWNLOAD_STALLED;
        case PullReadOutcome::READ_ERROR: return PullFailure::CONNECTION_LOST;
        case PullReadOutcome::OVERRUN: return PullFailure::BAD_REPLY;
    }
    return PullFailure::INTERNAL;
}
