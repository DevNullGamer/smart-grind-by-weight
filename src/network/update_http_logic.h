#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/update.h"
#include "../config/wifi.h"

// Pure helpers for the WiFi push-update HTTP endpoint (no Arduino/ESP dependencies, unit tested on the host).

//------------------------------------------------------------------------------
// Authorization header
//------------------------------------------------------------------------------
// Parses "Bearer <token>" (scheme case-insensitive, exactly one space, no trailing bytes) where the token is
// WIFI_TOKEN_LEN lowercase hex characters. out_token is NUL-terminated on success and cleared otherwise.
bool http_parse_bearer(const char* header_value, char out_token[WIFI_TOKEN_LEN + 1]);

//------------------------------------------------------------------------------
// Authentication failure lockout
//------------------------------------------------------------------------------
// UPDATE_AUTH_MAX_FAILURES bad tokens in a row lock authentication for a while; the lock doubles on each
// further lock (UPDATE_AUTH_LOCK_INITIAL_MS .. UPDATE_AUTH_LOCK_MAX_MS) and resets on a successful login.
// While locked, even a correct token is refused and further failures are not counted.
// All time maths is wrap-safe for millis() as long as the state is consulted at least every ~24 days.
struct AuthLockout {
    uint32_t failures = 0;       // consecutive bad tokens since the last lock / success
    uint32_t next_lock_ms = UPDATE_AUTH_LOCK_INITIAL_MS;
    uint32_t lock_until_ms = 0;
    bool locked = false;
};

// True while locked; clears an expired lock as a side effect.
bool auth_lockout_active(AuthLockout* state, uint32_t now_ms);
// Milliseconds until the lock ends (0 when not locked).
uint32_t auth_lockout_remaining_ms(AuthLockout* state, uint32_t now_ms);
// Records a bad token; returns true if this failure started a lock. Ignored while locked.
bool auth_lockout_record_failure(AuthLockout* state, uint32_t now_ms);
// A good token resets failures and the lock duration.
void auth_lockout_record_success(AuthLockout* state);

//------------------------------------------------------------------------------
// POST /api/update/begin body: {"size":N,"sha256":"64hex","build":N,"version":"..."}
//------------------------------------------------------------------------------
struct UpdateBeginRequest {
    uint32_t size = 0;
    char sha256[65] = {0};
    int32_t build = -1;          // -1: absent
    char version[UPDATE_VERSION_MAX_LEN + 1] = {0};  // empty: absent
};

enum class BeginParseResult : uint8_t {
    OK,
    BODY_EMPTY,
    BODY_TOO_LARGE,   // more than UPDATE_BEGIN_BODY_MAX bytes
    MALFORMED,        // not a flat JSON object of simple values, duplicate keys, escapes, trailing bytes
    MISSING_SIZE,
    SIZE_TOO_SMALL,   // below UPDATE_IMAGE_MIN_SIZE
    SIZE_TOO_LARGE,   // above the slot size
    BAD_SIZE,         // not a plain unsigned integer
    MISSING_SHA,
    BAD_SHA,          // not exactly 64 hex characters
    BAD_BUILD,        // not an integer in 0..2147483647
    BAD_VERSION       // not a string or longer than UPDATE_VERSION_MAX_LEN
};

// body is not NUL-terminated. Unknown keys are ignored if their value is a short string (<= 64 chars), number,
// true, false or null; nested objects/arrays are rejected. Fields are only filled on OK (otherwise *out is reset).
BeginParseResult http_parse_begin_request(const char* body, size_t len, uint32_t slot_size, UpdateBeginRequest* out);
const char* begin_parse_result_text(BeginParseResult result);

//------------------------------------------------------------------------------
// Session id (8 lowercase hex characters, drawn from the hardware RNG by the firmware layer)
//------------------------------------------------------------------------------
bool session_id_valid(const char* id);
void session_id_from_random(uint32_t random_value, char out[UPDATE_SESSION_ID_LEN + 1]);
// Constant-time; false if either id is not valid
bool session_id_equal(const char* a, const char* b);

//------------------------------------------------------------------------------
// Upload length rule: the PUT body must be exactly the announced image size
//------------------------------------------------------------------------------
enum class UploadLengthResult : uint8_t { OK, MISSING, MISMATCH };
UploadLengthResult upload_length_check(size_t content_length, uint32_t announced_size);
