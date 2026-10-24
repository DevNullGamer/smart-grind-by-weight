#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/update.h"

// Pure helpers for the release index (update-stable.json / update-beta.json) and SemVer 2.0 version
// comparison. No Arduino/ESP dependencies, unit tested on the host. The files are produced by
// tools/update_index.py; the shared SemVer vectors live in both test suites and must stay in step.

//------------------------------------------------------------------------------
// SemVer 2.0
//------------------------------------------------------------------------------
#define SEMVER_PRE_MAX UPDATE_VERSION_MAX_LEN   // Longest accepted version string (and so prerelease part)

struct Semver {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    bool has_pre = false;
    char pre[SEMVER_PRE_MAX + 1] = {0};          // prerelease identifiers without the leading '-'
};

// "MAJOR.MINOR.PATCH[-pre][+build]". Core numbers are plain decimals without leading zeros (up to 9 digits);
// prerelease identifiers are non-empty [0-9A-Za-z-], numeric ones without leading zeros; build metadata is
// validated but dropped. A leading "v", a partial core or trailing junk is rejected. *out is reset on failure.
bool semver_parse(const char* text, Semver* out);

// -1 / 0 / +1 by SemVer precedence (release > prerelease; dotted prerelease identifiers compared one by one,
// numeric < alphanumeric, shorter wins when all earlier ones are equal).
int semver_compare(const Semver& a, const Semver& b);

// True only if both strings parse and offered has strictly higher precedence than installed
// (an installed rc of the next version is never offered the older stable).
bool release_is_newer(const char* installed, const char* offered);

//------------------------------------------------------------------------------
// Index file: flat JSON object, see docs/DESIGN-NOTES.md ("Release index")
//   {"schema":1,"channel":"stable","version":"1.5.0","tag":"v1.5.0","prerelease":false,
//    "published":"2026-11-01","size":2240000,"sha256":"<64 hex>","url":"<base>firmware/v1.5.0/....bin"}
//   {"schema":1,"channel":"stable","none":true}      (no qualifying release)
//------------------------------------------------------------------------------
struct ReleaseInfo {
    char channel[8] = {0};                                   // "stable" or "beta"
    char version[SEMVER_PRE_MAX + 1] = {0};                  // without leading "v"
    char tag[SEMVER_PRE_MAX + 2] = {0};                      // "v" + version
    bool prerelease = false;
    char published[11] = {0};                                // YYYY-MM-DD
    uint32_t size = 0;
    char sha256[65] = {0};
    char url[UPDATE_PULL_STRING_MAX + 1] = {0};
};

enum class ReleaseParseResult : uint8_t {
    OK,               // a release is offered, *out is filled
    NONE,             // valid index without a release ("none":true)
    BODY_EMPTY,
    BODY_TOO_LARGE,   // more than UPDATE_PULL_INDEX_MAX_BYTES
    MALFORMED,        // not a flat JSON object of simple values, duplicate keys, escapes, trailing bytes
    BAD_SCHEMA,       // missing or not UPDATE_PULL_INDEX_SCHEMA
    BAD_CHANNEL,      // missing, not "stable" / "beta", not the expected channel, or a prerelease on stable
    MISSING_FIELD,    // version, tag, prerelease, published, size, sha256 or url absent
    BAD_VERSION,      // version/tag not SemVer, tag != "v" + version, prerelease flag inconsistent, string too long
    BAD_PUBLISHED,    // not YYYY-MM-DD
    BAD_SIZE,         // not a plain integer, below UPDATE_IMAGE_MIN_SIZE or above slot_size
    BAD_SHA,          // not exactly 64 hex characters
    BAD_URL           // not UPDATE_PULL_BASE_URL + [A-Za-z0-9._/-]+ + ".bin", or too long
};

// expected_channel ("stable" or "beta", the file that was fetched) must equal the document's channel, and a
// stable document must not be a prerelease; both mismatches give BAD_CHANNEL.
// body is not NUL-terminated. Unknown keys are ignored if their value is a short string, number, true, false or
// null (nested values are rejected). *out is reset unless the result is OK.
ReleaseParseResult release_parse_index(const char* body, size_t len, uint32_t slot_size, const char* expected_channel,
                                       ReleaseInfo* out);
const char* release_parse_result_text(ReleaseParseResult result);
