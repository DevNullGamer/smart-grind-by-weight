#include "update_http_logic.h"
#include "../system/sha_hex.h"
#include "wifi_credentials.h"
#include <string.h>

//------------------------------------------------------------------------------
// Authorization header
//------------------------------------------------------------------------------
bool http_parse_bearer(const char* header_value, char out_token[WIFI_TOKEN_LEN + 1]) {
    if (out_token) out_token[0] = '\0';
    if (!header_value || !out_token) return false;
    static const char kScheme[] = "Bearer ";
    const size_t scheme_len = sizeof(kScheme) - 1;
    for (size_t i = 0; i < scheme_len; i++) {
        char c = header_value[i];
        if (c == '\0') return false;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        char want = kScheme[i];
        if (want >= 'A' && want <= 'Z') want = static_cast<char>(want - 'A' + 'a');
        if (c != want) return false;
    }
    const char* token = header_value + scheme_len;
    // Exactly WIFI_TOKEN_LEN characters and then the end of the value
    if (strnlen(token, WIFI_TOKEN_LEN + 1) != WIFI_TOKEN_LEN) return false;
    char tmp[WIFI_TOKEN_LEN + 1];
    memcpy(tmp, token, WIFI_TOKEN_LEN);
    tmp[WIFI_TOKEN_LEN] = '\0';
    if (!wifi_token_is_valid(tmp)) return false;
    memcpy(out_token, tmp, sizeof(tmp));
    return true;
}

//------------------------------------------------------------------------------
// Authentication failure lockout
//------------------------------------------------------------------------------
bool auth_lockout_active(AuthLockout* state, uint32_t now_ms) {
    if (!state->locked) return false;
    if (static_cast<int32_t>(state->lock_until_ms - now_ms) > 0) return true;
    state->locked = false;
    return false;
}

uint32_t auth_lockout_remaining_ms(AuthLockout* state, uint32_t now_ms) {
    if (!auth_lockout_active(state, now_ms)) return 0;
    return state->lock_until_ms - now_ms;
}

bool auth_lockout_record_failure(AuthLockout* state, uint32_t now_ms) {
    if (auth_lockout_active(state, now_ms)) return false;
    state->failures++;
    if (state->failures < UPDATE_AUTH_MAX_FAILURES) return false;
    state->failures = 0;
    state->locked = true;
    state->lock_until_ms = now_ms + state->next_lock_ms;
    state->next_lock_ms = state->next_lock_ms >= UPDATE_AUTH_LOCK_MAX_MS / 2 ? UPDATE_AUTH_LOCK_MAX_MS
                                                                             : state->next_lock_ms * 2;
    return true;
}

void auth_lockout_record_success(AuthLockout* state) {
    state->failures = 0;
    state->next_lock_ms = UPDATE_AUTH_LOCK_INITIAL_MS;
    state->locked = false;
}

//------------------------------------------------------------------------------
// Begin request parser
//------------------------------------------------------------------------------
namespace {

constexpr size_t kMaxKeyLen = 32;
constexpr size_t kMaxUnknownStringLen = 64;

struct Cursor {
    const char* p;
    const char* end;
    bool at_end() const { return p >= end; }
    char peek() const { return *p; }
};

void skip_ws(Cursor& c) {
    while (!c.at_end() && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n')) c.p++;
}

// Cursor is on the opening quote. Plain printable ASCII only: no escapes, control or non-ASCII bytes.
bool parse_string(Cursor& c, const char** start, size_t* length) {
    if (c.at_end() || *c.p != '"') return false;
    c.p++;
    *start = c.p;
    while (!c.at_end() && *c.p != '"') {
        const unsigned char ch = static_cast<unsigned char>(*c.p);
        if (ch < 0x20 || ch > 0x7E || ch == '\\') return false;
        c.p++;
    }
    if (c.at_end()) return false;
    *length = static_cast<size_t>(c.p - *start);
    c.p++;  // closing quote
    return true;
}

// Bare number token as it appears in the text (digits, sign, '.', exponent)
bool parse_number_token(Cursor& c, const char** start, size_t* length) {
    *start = c.p;
    while (!c.at_end() && ((*c.p >= '0' && *c.p <= '9') || *c.p == '-' || *c.p == '+' || *c.p == '.' ||
                           *c.p == 'e' || *c.p == 'E')) {
        c.p++;
    }
    *length = static_cast<size_t>(c.p - *start);
    return *length > 0 && *length <= 24;
}

bool match_literal(Cursor& c, const char* word) {
    const size_t n = strlen(word);
    if (static_cast<size_t>(c.end - c.p) < n || memcmp(c.p, word, n) != 0) return false;
    c.p += n;
    return true;
}

// Unsigned decimal without sign, fraction, exponent or leading zeros; at most max_value
bool parse_uint(const char* s, size_t n, uint64_t max_value, uint64_t* out) {
    if (n == 0 || n > 10) return false;
    if (n > 1 && s[0] == '0') return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    }
    if (v > max_value) return false;
    *out = v;
    return true;
}

bool key_is(const char* key, size_t key_len, const char* name) {
    return strlen(name) == key_len && memcmp(key, name, key_len) == 0;
}

}  // namespace

BeginParseResult http_parse_begin_request(const char* body, size_t len, uint32_t slot_size, UpdateBeginRequest* out) {
    if (!out) return BeginParseResult::MALFORMED;
    *out = UpdateBeginRequest();
    if (!body || len == 0) return BeginParseResult::BODY_EMPTY;
    if (len > UPDATE_BEGIN_BODY_MAX) return BeginParseResult::BODY_TOO_LARGE;

    UpdateBeginRequest req;
    Cursor c{body, body + len};
    bool seen_size = false, seen_sha = false, seen_build = false, seen_version = false;

    skip_ws(c);
    if (c.at_end() || *c.p != '{') return BeginParseResult::MALFORMED;
    c.p++;
    skip_ws(c);

    bool first = true;
    while (true) {
        if (c.at_end()) return BeginParseResult::MALFORMED;
        if (*c.p == '}') {
            if (!first) return BeginParseResult::MALFORMED;  // trailing comma
            c.p++;
            break;
        }
        const char* key;
        size_t key_len;
        if (!parse_string(c, &key, &key_len) || key_len == 0 || key_len > kMaxKeyLen) return BeginParseResult::MALFORMED;
        skip_ws(c);
        if (c.at_end() || *c.p != ':') return BeginParseResult::MALFORMED;
        c.p++;
        skip_ws(c);
        if (c.at_end()) return BeginParseResult::MALFORMED;

        if (key_is(key, key_len, "size")) {
            if (seen_size) return BeginParseResult::MALFORMED;
            seen_size = true;
            const char* num;
            size_t n;
            if (*c.p == '"' || !parse_number_token(c, &num, &n)) return BeginParseResult::BAD_SIZE;
            uint64_t v;
            if (!parse_uint(num, n, 0xFFFFFFFFull, &v)) return BeginParseResult::BAD_SIZE;
            if (v < UPDATE_IMAGE_MIN_SIZE) return BeginParseResult::SIZE_TOO_SMALL;
            if (v > slot_size) return BeginParseResult::SIZE_TOO_LARGE;
            req.size = static_cast<uint32_t>(v);
        } else if (key_is(key, key_len, "sha256")) {
            if (seen_sha) return BeginParseResult::MALFORMED;
            seen_sha = true;
            const char* s;
            size_t n;
            if (*c.p != '"' || !parse_string(c, &s, &n)) return BeginParseResult::BAD_SHA;
            if (n != SHA256_HEX_LEN) return BeginParseResult::BAD_SHA;
            char tmp[SHA256_HEX_LEN + 1];
            memcpy(tmp, s, n);
            tmp[n] = '\0';
            uint8_t digest[SHA256_DIGEST_LEN];
            if (!sha256_hex_parse(tmp, digest)) return BeginParseResult::BAD_SHA;
            memcpy(req.sha256, tmp, sizeof(tmp));
        } else if (key_is(key, key_len, "build")) {
            if (seen_build) return BeginParseResult::MALFORMED;
            seen_build = true;
            const char* num;
            size_t n;
            if (*c.p == '"' || !parse_number_token(c, &num, &n)) return BeginParseResult::BAD_BUILD;
            uint64_t v;
            if (!parse_uint(num, n, 0x7FFFFFFFull, &v)) return BeginParseResult::BAD_BUILD;
            req.build = static_cast<int32_t>(v);
        } else if (key_is(key, key_len, "version")) {
            if (seen_version) return BeginParseResult::MALFORMED;
            seen_version = true;
            const char* s;
            size_t n;
            if (*c.p != '"') return BeginParseResult::BAD_VERSION;
            if (!parse_string(c, &s, &n)) return BeginParseResult::MALFORMED;
            if (n > UPDATE_VERSION_MAX_LEN) return BeginParseResult::BAD_VERSION;
            memcpy(req.version, s, n);
            req.version[n] = '\0';
        } else {
            // Unknown key: tolerate small flat values only
            if (*c.p == '"') {
                const char* s;
                size_t n;
                if (!parse_string(c, &s, &n) || n > kMaxUnknownStringLen) return BeginParseResult::MALFORMED;
            } else if (*c.p == 't') {
                if (!match_literal(c, "true")) return BeginParseResult::MALFORMED;
            } else if (*c.p == 'f') {
                if (!match_literal(c, "false")) return BeginParseResult::MALFORMED;
            } else if (*c.p == 'n') {
                if (!match_literal(c, "null")) return BeginParseResult::MALFORMED;
            } else {
                const char* num;
                size_t n;
                if (!parse_number_token(c, &num, &n)) return BeginParseResult::MALFORMED;
            }
        }

        first = false;
        skip_ws(c);
        if (c.at_end()) return BeginParseResult::MALFORMED;
        if (*c.p == ',') {
            c.p++;
            skip_ws(c);
            continue;
        }
        if (*c.p == '}') {
            c.p++;
            break;
        }
        return BeginParseResult::MALFORMED;
    }

    skip_ws(c);
    if (!c.at_end()) return BeginParseResult::MALFORMED;  // trailing bytes
    if (!seen_size) return BeginParseResult::MISSING_SIZE;
    if (!seen_sha) return BeginParseResult::MISSING_SHA;
    *out = req;
    return BeginParseResult::OK;
}

const char* begin_parse_result_text(BeginParseResult result) {
    switch (result) {
        case BeginParseResult::OK: return "ok";
        case BeginParseResult::BODY_EMPTY: return "empty request body";
        case BeginParseResult::BODY_TOO_LARGE: return "request body too large";
        case BeginParseResult::MALFORMED: return "malformed JSON body";
        case BeginParseResult::MISSING_SIZE: return "size missing";
        case BeginParseResult::SIZE_TOO_SMALL: return "image too small";
        case BeginParseResult::SIZE_TOO_LARGE: return "image too large";
        case BeginParseResult::BAD_SIZE: return "size must be a plain integer";
        case BeginParseResult::MISSING_SHA: return "sha256 missing";
        case BeginParseResult::BAD_SHA: return "sha256 must be 64 hex characters";
        case BeginParseResult::BAD_BUILD: return "build must be a non-negative integer";
        case BeginParseResult::BAD_VERSION: return "version must be a string of at most 32 characters";
    }
    return "bad request";
}

//------------------------------------------------------------------------------
// Session id
//------------------------------------------------------------------------------
bool session_id_valid(const char* id) {
    if (!id || strnlen(id, UPDATE_SESSION_ID_LEN + 1) != UPDATE_SESSION_ID_LEN) return false;
    for (size_t i = 0; i < UPDATE_SESSION_ID_LEN; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

void session_id_from_random(uint32_t random_value, char out[UPDATE_SESSION_ID_LEN + 1]) {
    static const char kDigits[] = "0123456789abcdef";
    for (int i = 0; i < UPDATE_SESSION_ID_LEN; i++) {
        out[UPDATE_SESSION_ID_LEN - 1 - i] = kDigits[(random_value >> (4 * i)) & 0xF];
    }
    out[UPDATE_SESSION_ID_LEN] = '\0';
}

bool session_id_equal(const char* a, const char* b) {
    if (!session_id_valid(a) || !session_id_valid(b)) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < UPDATE_SESSION_ID_LEN; i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

//------------------------------------------------------------------------------
// Upload length
//------------------------------------------------------------------------------
UploadLengthResult upload_length_check(size_t content_length, uint32_t announced_size) {
    if (content_length == 0) return UploadLengthResult::MISSING;
    if (content_length != announced_size) return UploadLengthResult::MISMATCH;
    return UploadLengthResult::OK;
}
