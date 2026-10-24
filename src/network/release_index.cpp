#include "release_index.h"
#include "../system/sha_hex.h"
#include <string.h>

//------------------------------------------------------------------------------
// SemVer 2.0
//------------------------------------------------------------------------------
namespace {

constexpr size_t kMaxCoreDigits = 9;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_char(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'; }

// Next dot-separated identifier at p ('.', '+' or NUL ends it). False when it is empty or has a bad character.
bool scan_identifier(const char* p, size_t* length) {
    size_t n = 0;
    while (is_ident_char(p[n])) n++;
    *length = n;
    return n > 0;
}

bool identifier_is_numeric(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) return false;
    }
    return true;
}

// Validates "id(.id)*" up to '+' or NUL; leading zeros are only forbidden in numeric prerelease identifiers.
bool scan_identifier_list(const char*& p, bool forbid_numeric_leading_zero) {
    while (true) {
        size_t n;
        if (!scan_identifier(p, &n)) return false;
        if (forbid_numeric_leading_zero && n > 1 && p[0] == '0' && identifier_is_numeric(p, n)) return false;
        p += n;
        if (*p != '.') return true;
        p++;
    }
}

bool parse_core_number(const char*& p, uint32_t* out) {
    size_t n = 0;
    uint32_t v = 0;
    while (is_digit(p[n])) {
        if (n >= kMaxCoreDigits) return false;
        v = v * 10 + static_cast<uint32_t>(p[n] - '0');
        n++;
    }
    if (n == 0 || (n > 1 && p[0] == '0')) return false;
    p += n;
    *out = v;
    return true;
}

// Numeric identifiers by value (no leading zeros, so length first), alphanumeric ones in ASCII order,
// numeric below alphanumeric.
int compare_identifiers(const char* a, size_t an, const char* b, size_t bn) {
    const bool a_num = identifier_is_numeric(a, an);
    const bool b_num = identifier_is_numeric(b, bn);
    if (a_num != b_num) return a_num ? -1 : 1;
    if (a_num && an != bn) return an < bn ? -1 : 1;
    const int c = memcmp(a, b, an < bn ? an : bn);
    if (c != 0) return c < 0 ? -1 : 1;
    if (an == bn) return 0;
    return an < bn ? -1 : 1;
}

int compare_u32(uint32_t a, uint32_t b) { return a == b ? 0 : (a < b ? -1 : 1); }

}  // namespace

bool semver_parse(const char* text, Semver* out) {
    if (!out) return false;
    *out = Semver();
    if (!text || strnlen(text, SEMVER_PRE_MAX + 1) > SEMVER_PRE_MAX) return false;

    Semver v;
    const char* p = text;
    if (!parse_core_number(p, &v.major) || *p++ != '.') return false;
    if (!parse_core_number(p, &v.minor) || *p++ != '.') return false;
    if (!parse_core_number(p, &v.patch)) return false;

    if (*p == '-') {
        p++;
        const char* start = p;
        if (!scan_identifier_list(p, true)) return false;
        const size_t n = static_cast<size_t>(p - start);
        memcpy(v.pre, start, n);
        v.pre[n] = '\0';
        v.has_pre = true;
    }
    if (*p == '+') {
        p++;
        if (!scan_identifier_list(p, false)) return false;  // validated, then dropped
    }
    if (*p != '\0') return false;
    *out = v;
    return true;
}

int semver_compare(const Semver& a, const Semver& b) {
    int c = compare_u32(a.major, b.major);
    if (c != 0) return c;
    c = compare_u32(a.minor, b.minor);
    if (c != 0) return c;
    c = compare_u32(a.patch, b.patch);
    if (c != 0) return c;

    if (a.has_pre != b.has_pre) return a.has_pre ? -1 : 1;  // a release outranks its own prereleases
    if (!a.has_pre) return 0;

    const char* pa = a.pre;
    const char* pb = b.pre;
    while (true) {
        if (*pa == '\0' || *pb == '\0') {
            if (*pa == *pb) return 0;
            return *pa == '\0' ? -1 : 1;  // the shorter list is lower
        }
        size_t an, bn;
        scan_identifier(pa, &an);
        scan_identifier(pb, &bn);
        c = compare_identifiers(pa, an, pb, bn);
        if (c != 0) return c;
        pa += an;
        pb += bn;
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
    }
}

bool release_is_newer(const char* installed, const char* offered) {
    Semver have, want;
    if (!semver_parse(installed, &have) || !semver_parse(offered, &want)) return false;
    return semver_compare(want, have) > 0;
}

//------------------------------------------------------------------------------
// Index file parser (same flat-JSON style as http_parse_begin_request)
//------------------------------------------------------------------------------
namespace {

constexpr size_t kMaxKeyLen = 32;
constexpr size_t kPublishedLen = 10;

struct Cursor {
    const char* p;
    const char* end;
    bool at_end() const { return p >= end; }
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
    return *length <= UPDATE_PULL_STRING_MAX;
}

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

struct StringField {
    const char* s = nullptr;
    size_t n = 0;
    bool seen = false;
};

bool string_equals(const StringField& f, const char* text) {
    return f.n == strlen(text) && memcmp(f.s, text, f.n) == 0;
}

// Parses a boolean literal value for a known key
bool parse_bool(Cursor& c, bool* out) {
    if (match_literal(c, "true")) {
        *out = true;
        return true;
    }
    if (match_literal(c, "false")) {
        *out = false;
        return true;
    }
    return false;
}

bool starts_with(const StringField& f, const char* prefix) {
    const size_t n = strlen(prefix);
    return f.n >= n && memcmp(f.s, prefix, n) == 0;
}

bool ends_with(const StringField& f, const char* suffix) {
    const size_t n = strlen(suffix);
    return f.n >= n && memcmp(f.s + f.n - n, suffix, n) == 0;
}

bool contains_dotdot(const StringField& f) {
    for (size_t i = 0; i + 1 < f.n; i++) {
        if (f.s[i] == '.' && f.s[i + 1] == '.') return true;
    }
    return false;
}

// Copies a bounded field into a NUL-terminated buffer
void copy_field(const StringField& f, char* dst, size_t dst_size) {
    const size_t n = f.n < dst_size - 1 ? f.n : dst_size - 1;
    memcpy(dst, f.s, n);
    dst[n] = '\0';
}

// Path below the base URL: [A-Za-z0-9._/-] only (no query, fragment, userinfo, port or percent escapes)
bool url_tail_is_valid(const StringField& f, size_t base_len) {
    for (size_t i = base_len; i < f.n; i++) {
        const char c = f.s[i];
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '/' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

bool published_is_valid(const StringField& f) {
    if (f.n != kPublishedLen) return false;
    for (size_t i = 0; i < f.n; i++) {
        if (i == 4 || i == 7) {
            if (f.s[i] != '-') return false;
        } else if (!is_digit(f.s[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

ReleaseParseResult release_parse_index(const char* body, size_t len, uint32_t slot_size, const char* expected_channel,
                                       ReleaseInfo* out) {
    if (!out) return ReleaseParseResult::MALFORMED;
    *out = ReleaseInfo();
    if (!expected_channel) return ReleaseParseResult::BAD_CHANNEL;
    if (!body || len == 0) return ReleaseParseResult::BODY_EMPTY;
    if (len > UPDATE_PULL_INDEX_MAX_BYTES) return ReleaseParseResult::BODY_TOO_LARGE;

    Cursor c{body, body + len};
    StringField channel, version, tag, published, sha, url;
    bool seen_schema = false, seen_none = false, seen_prerelease = false, seen_size = false;
    bool none = false, prerelease = false;
    uint64_t schema = 0, size = 0;
    bool size_ok = false;

    skip_ws(c);
    if (c.at_end() || *c.p != '{') return ReleaseParseResult::MALFORMED;
    c.p++;
    skip_ws(c);

    bool first = true;
    while (true) {
        if (c.at_end()) return ReleaseParseResult::MALFORMED;
        if (*c.p == '}') {
            if (!first) return ReleaseParseResult::MALFORMED;  // trailing comma
            c.p++;
            break;
        }
        const char* key;
        size_t key_len;
        if (!parse_string(c, &key, &key_len) || key_len == 0 || key_len > kMaxKeyLen) {
            return ReleaseParseResult::MALFORMED;
        }
        skip_ws(c);
        if (c.at_end() || *c.p != ':') return ReleaseParseResult::MALFORMED;
        c.p++;
        skip_ws(c);
        if (c.at_end()) return ReleaseParseResult::MALFORMED;

        StringField* string_target = nullptr;
        if (key_is(key, key_len, "channel")) string_target = &channel;
        else if (key_is(key, key_len, "version")) string_target = &version;
        else if (key_is(key, key_len, "tag")) string_target = &tag;
        else if (key_is(key, key_len, "published")) string_target = &published;
        else if (key_is(key, key_len, "sha256")) string_target = &sha;
        else if (key_is(key, key_len, "url")) string_target = &url;

        if (string_target) {
            if (string_target->seen) return ReleaseParseResult::MALFORMED;
            string_target->seen = true;
            if (*c.p != '"' || !parse_string(c, &string_target->s, &string_target->n)) {
                return ReleaseParseResult::MALFORMED;
            }
        } else if (key_is(key, key_len, "schema")) {
            if (seen_schema) return ReleaseParseResult::MALFORMED;
            seen_schema = true;
            const char* num;
            size_t n;
            if (*c.p == '"' || !parse_number_token(c, &num, &n)) return ReleaseParseResult::BAD_SCHEMA;
            if (!parse_uint(num, n, 0xFFFFFFFFull, &schema)) return ReleaseParseResult::BAD_SCHEMA;
        } else if (key_is(key, key_len, "size")) {
            if (seen_size) return ReleaseParseResult::MALFORMED;
            seen_size = true;
            const char* num;
            size_t n;
            if (*c.p == '"' || !parse_number_token(c, &num, &n)) return ReleaseParseResult::BAD_SIZE;
            size_ok = parse_uint(num, n, 0xFFFFFFFFull, &size);
        } else if (key_is(key, key_len, "none")) {
            if (seen_none) return ReleaseParseResult::MALFORMED;
            seen_none = true;
            if (!parse_bool(c, &none)) return ReleaseParseResult::MALFORMED;
        } else if (key_is(key, key_len, "prerelease")) {
            if (seen_prerelease) return ReleaseParseResult::MALFORMED;
            seen_prerelease = true;
            if (!parse_bool(c, &prerelease)) return ReleaseParseResult::MALFORMED;
        } else {
            // Unknown key: tolerate small flat values only
            if (*c.p == '"') {
                const char* s;
                size_t n;
                if (!parse_string(c, &s, &n)) return ReleaseParseResult::MALFORMED;
            } else if (*c.p == 't') {
                if (!match_literal(c, "true")) return ReleaseParseResult::MALFORMED;
            } else if (*c.p == 'f') {
                if (!match_literal(c, "false")) return ReleaseParseResult::MALFORMED;
            } else if (*c.p == 'n') {
                if (!match_literal(c, "null")) return ReleaseParseResult::MALFORMED;
            } else {
                const char* num;
                size_t n;
                if (!parse_number_token(c, &num, &n)) return ReleaseParseResult::MALFORMED;
            }
        }

        first = false;
        skip_ws(c);
        if (c.at_end()) return ReleaseParseResult::MALFORMED;
        if (*c.p == ',') {
            c.p++;
            skip_ws(c);
            continue;
        }
        if (*c.p == '}') {
            c.p++;
            break;
        }
        return ReleaseParseResult::MALFORMED;
    }

    skip_ws(c);
    if (!c.at_end()) return ReleaseParseResult::MALFORMED;  // trailing bytes

    if (!seen_schema || schema != UPDATE_PULL_INDEX_SCHEMA) return ReleaseParseResult::BAD_SCHEMA;
    if (!channel.seen || !(string_equals(channel, "stable") || string_equals(channel, "beta"))) {
        return ReleaseParseResult::BAD_CHANNEL;
    }
    if (!string_equals(channel, expected_channel)) return ReleaseParseResult::BAD_CHANNEL;

    if (seen_none) {
        // The "no release" document carries nothing but schema, channel and none:true
        const bool has_release_key = version.seen || tag.seen || seen_prerelease || published.seen || seen_size ||
                                     sha.seen || url.seen;
        if (!none || has_release_key) return ReleaseParseResult::MALFORMED;
        return ReleaseParseResult::NONE;
    }

    if (!version.seen || !tag.seen || !seen_prerelease || !published.seen || !seen_size || !sha.seen || !url.seen) {
        return ReleaseParseResult::MISSING_FIELD;
    }

    // version / tag
    char version_buf[UPDATE_PULL_STRING_MAX + 1];
    char tag_buf[UPDATE_PULL_STRING_MAX + 1];
    copy_field(version, version_buf, sizeof(version_buf));
    copy_field(tag, tag_buf, sizeof(tag_buf));
    Semver parsed;
    if (version.n > SEMVER_PRE_MAX || !semver_parse(version_buf, &parsed)) return ReleaseParseResult::BAD_VERSION;
    if (tag.n != version.n + 1 || tag.s[0] != 'v' || memcmp(tag.s + 1, version.s, version.n) != 0) {
        return ReleaseParseResult::BAD_VERSION;
    }
    if (prerelease != parsed.has_pre) return ReleaseParseResult::BAD_VERSION;
    if (prerelease && string_equals(channel, "stable")) return ReleaseParseResult::BAD_CHANNEL;

    if (!published_is_valid(published)) return ReleaseParseResult::BAD_PUBLISHED;

    if (!size_ok || size < UPDATE_IMAGE_MIN_SIZE || size > slot_size) return ReleaseParseResult::BAD_SIZE;

    if (sha.n != SHA256_HEX_LEN) return ReleaseParseResult::BAD_SHA;
    char sha_buf[SHA256_HEX_LEN + 1];
    copy_field(sha, sha_buf, sizeof(sha_buf));
    uint8_t digest[SHA256_DIGEST_LEN];
    if (!sha256_hex_parse(sha_buf, digest)) return ReleaseParseResult::BAD_SHA;

    const size_t base_len = strlen(UPDATE_PULL_BASE_URL);
    if (!starts_with(url, UPDATE_PULL_BASE_URL) || !ends_with(url, ".bin") || url.n <= base_len + 4 ||
        contains_dotdot(url) || !url_tail_is_valid(url, base_len)) {
        return ReleaseParseResult::BAD_URL;
    }

    ReleaseInfo info;
    copy_field(channel, info.channel, sizeof(info.channel));
    memcpy(info.version, version_buf, version.n + 1);
    memcpy(info.tag, tag_buf, tag.n + 1);
    info.prerelease = prerelease;
    copy_field(published, info.published, sizeof(info.published));
    info.size = static_cast<uint32_t>(size);
    memcpy(info.sha256, sha_buf, sizeof(sha_buf));
    copy_field(url, info.url, sizeof(info.url));
    *out = info;
    return ReleaseParseResult::OK;
}

const char* release_parse_result_text(ReleaseParseResult result) {
    switch (result) {
        case ReleaseParseResult::OK: return "ok";
        case ReleaseParseResult::NONE: return "no release published";
        case ReleaseParseResult::BODY_EMPTY: return "empty index";
        case ReleaseParseResult::BODY_TOO_LARGE: return "index too large";
        case ReleaseParseResult::MALFORMED: return "malformed index";
        case ReleaseParseResult::BAD_SCHEMA: return "unsupported index schema";
        case ReleaseParseResult::BAD_CHANNEL: return "bad channel";
        case ReleaseParseResult::MISSING_FIELD: return "index field missing";
        case ReleaseParseResult::BAD_VERSION: return "bad version";
        case ReleaseParseResult::BAD_PUBLISHED: return "bad publish date";
        case ReleaseParseResult::BAD_SIZE: return "bad image size";
        case ReleaseParseResult::BAD_SHA: return "sha256 must be 64 hex characters";
        case ReleaseParseResult::BAD_URL: return "bad image url";
    }
    return "bad index";
}
