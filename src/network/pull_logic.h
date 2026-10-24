#pragma once
#include <stddef.h>
#include <stdint.h>
#include "release_index.h"

// Pure decisions of the HTTPS pull update (no Arduino/ESP dependencies, unit tested on the host):
// job states and their transitions, failure reasons and their display texts, the precondition order, the
// verdict on an index file and the checks on an HTTP response. The IDF side is network/update_pull.* and
// network/release_client.*.

enum class PullState : uint8_t {
    IDLE,
    CHECKING,       // worker is fetching and evaluating the index
    UP_TO_DATE,     // installed version is the newest (or no release is published)
    AVAILABLE,      // a newer release is known; request_install() may start
    CHECK_FAILED,   // the check did not complete; failure() says why
    DOWNLOADING,    // image download + flash write
    VERIFYING,      // all bytes received, SHA-256 + image check
    INSTALLED,      // new image selected, the device restarts
    FAILED          // the install did not complete; failure() says why; old image untouched
};

enum class PullFailure : uint8_t {
    NONE,
    // Refusals before anything started
    BUSY,                 // a check or install is already running
    NOT_AVAILABLE,        // install without a fresh AVAILABLE result
    NO_WIFI,
    UPDATE_RUNNING,       // push / BLE update or accept prompt in progress
    GRINDER_BUSY,
    BLE_CONNECTED,
    VERIFYING_FIRMWARE,   // running image not confirmed yet (boot health), esp_ota_begin would fail
    LOW_MEMORY,
    // Check / download failures
    NO_CONNECTION,        // DNS / TCP / TLS / no response
    NOT_FOUND,            // HTTP 404
    SERVER_ERROR,         // any other status than 200
    BAD_REPLY,            // no Content-Length (chunked), or it differs from what the index promised
    REPLY_TOO_LARGE,
    INDEX_INVALID,        // index file failed the parser
    DOWNLOAD_STALLED,     // no data within the read timeout
    CONNECTION_LOST,      // read error
    DOWNLOAD_INTERRUPTED, // server closed early
    // Sink / install failures
    CHECKSUM_MISMATCH,
    NOT_FIRMWARE,         // header check of the first bytes failed
    IMAGE_REJECTED,       // size / structure rejected by the sink or esp_ota_end
    FLASH_FAILED,
    INTERNAL
};

// Short, display-ready reason (fits the progress screen; no trailing period)
const char* pull_failure_text(PullFailure failure);
const char* pull_state_name(PullState state);

// True while a worker task exists for this state
bool pull_state_is_running(PullState state);
// Allowed state changes (the worker and the request functions only ever make these)
bool pull_transition_allowed(PullState from, PullState to);
// Which states accept a new check / an install request
bool pull_can_start_check(PullState state);
bool pull_can_start_install(PullState state);

//------------------------------------------------------------------------------
// Preconditions (facts are gathered by the caller; the order of the verdict is fixed here)
//------------------------------------------------------------------------------
struct PullPreconditions {
    bool wifi_connected = false;
    bool update_active = false;     // UpdateGuard held, accept prompt in use or a WiFi update in progress
    bool boot_pending = false;      // running image still PENDING_VERIFY
    bool grinder_busy = false;
    bool ble_connected = false;     // client connected or data export running
    uint32_t internal_free = 0;     // free internal heap bytes
};

// PullFailure::NONE when the request may start. A check only needs WiFi, no update in progress, an idle grinder and
// memory; an install additionally needs a confirmed running image and no Bluetooth client. State is checked first
// (BUSY while running, NOT_AVAILABLE for an install without AVAILABLE).
PullFailure pull_refusal(PullState state, bool for_install, const PullPreconditions& facts);

//------------------------------------------------------------------------------
// Index verdict
//------------------------------------------------------------------------------
struct PullIndexDecision {
    PullState state = PullState::CHECK_FAILED;   // UP_TO_DATE, AVAILABLE or CHECK_FAILED
    PullFailure failure = PullFailure::NONE;     // INDEX_INVALID for CHECK_FAILED
    bool no_release = false;                     // UP_TO_DATE because the channel is empty
};

// info is only read for ReleaseParseResult::OK. A release that is not strictly newer than installed_version (or
// either one not SemVer) is UP_TO_DATE: a downgrade is never offered.
PullIndexDecision pull_evaluate_index(ReleaseParseResult parsed, const ReleaseInfo* info, const char* installed_version);

//------------------------------------------------------------------------------
// HTTP response checks
//------------------------------------------------------------------------------
// https:// always, http:// only when allow_http (dev builds)
bool pull_url_scheme_allowed(const char* url, bool allow_http);

enum class PullHttpCheck : uint8_t { OK, BAD_STATUS, NO_LENGTH, TOO_LARGE, LENGTH_MISMATCH };
// Status must be 200, Content-Length known and positive (chunked or absent gives <= 0), not above max_length, and
// equal to expected_length when that is >= 0.
PullHttpCheck pull_check_response(int status, int64_t content_length, int64_t expected_length, int64_t max_length);
PullFailure pull_failure_for_http_check(PullHttpCheck check, int status);

// True once a download has run longer than max_ms (wrap-safe unsigned difference)
bool pull_download_deadline_exceeded(uint32_t started_ms, uint32_t now_ms, uint32_t max_ms);

// Result of one esp_http_client_read() call while a body of `total` bytes is expected
enum class PullReadOutcome : uint8_t { DATA, DONE, TRUNCATED, TIMEOUT, READ_ERROR, OVERRUN };
// n > 0: DATA (OVERRUN if it would pass `total`); 0: DONE when received == total, else TRUNCATED;
// -0x7007 (ESP_ERR_HTTP_EAGAIN, timed out): TIMEOUT; other negative: ERROR.
PullReadOutcome pull_classify_read(int n, uint32_t received, uint32_t total);
PullFailure pull_failure_for_read(PullReadOutcome outcome);
