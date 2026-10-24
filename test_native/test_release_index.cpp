#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "network/release_index.h"

static const uint32_t kSlot = 0x300000;

//------------------------------------------------------------------------------
// Shared SemVer vectors. tools/tests/test_update_index.py carries the identical lists:
// change both together.
//------------------------------------------------------------------------------
// Strictly ascending precedence
static const char* const kAscending[] = {
    "0.9.9",
    "1.0.0-alpha",
    "1.0.0-alpha.1",
    "1.0.0-alpha.beta",
    "1.0.0-beta",
    "1.0.0-beta.2",
    "1.0.0-beta.11",
    "1.0.0-rc.1",
    "1.0.0",
    "1.4.1-rc.3",
    "1.4.1-rc.10",
    "1.4.1",
    "1.5.0-beta",
    "1.5.0",
    "1.10.0",
    "2.0.0",
};
// Equal precedence (build metadata ignored)
static const char* const kEqual[][2] = {
    {"1.5.0", "1.5.0+build.7"},
    {"1.5.0+a", "1.5.0+b"},
    {"1.5.0-rc.1+x", "1.5.0-rc.1"},
};
// Not valid versions
static const char* const kInvalid[] = {
    "",
    "v1.5.0",
    "1.5",
    "1.5.x",
    "1.5.0.1",
    "1.05.0",
    "1.5.0-",
    "1.5.0-01",
    "1.5.0-rc..1",
    "1.5.0-rc.",
    "1.5.0+",
    "1.5.0 ",
    " 1.5.0",
    "-1.5.0",
    "1.5.0-rc_1",
    "1.5.0\n",
    "1234567890.0.0",
};

static Semver parse_ok(const char* text) {
    Semver v;
    TEST_ASSERT_TRUE_MESSAGE(semver_parse(text, &v), text);
    return v;
}

static void test_semver_ascending_order() {
    const size_t n = sizeof(kAscending) / sizeof(kAscending[0]);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            const int want = i == j ? 0 : (i < j ? -1 : 1);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s vs %s", kAscending[i], kAscending[j]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(want, semver_compare(parse_ok(kAscending[i]), parse_ok(kAscending[j])), msg);
        }
    }
}

static void test_semver_equal_ignores_build() {
    for (const auto& pair : kEqual) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, semver_compare(parse_ok(pair[0]), parse_ok(pair[1])), pair[0]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, semver_compare(parse_ok(pair[1]), parse_ok(pair[0])), pair[1]);
    }
}

static void test_semver_rejects_invalid() {
    for (const char* text : kInvalid) {
        Semver v;
        v.major = 7;
        TEST_ASSERT_FALSE_MESSAGE(semver_parse(text, &v), text);
        TEST_ASSERT_EQUAL_UINT32(0, v.major);  // reset on failure
    }
    Semver v;
    TEST_ASSERT_FALSE(semver_parse(nullptr, &v));
    TEST_ASSERT_FALSE(semver_parse("1.0.0", nullptr));
    // Longer than UPDATE_VERSION_MAX_LEN
    TEST_ASSERT_FALSE(semver_parse("1.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &v));
}

static void test_semver_fields() {
    const Semver v = parse_ok("12.3.456-rc.10+meta");
    TEST_ASSERT_EQUAL_UINT32(12, v.major);
    TEST_ASSERT_EQUAL_UINT32(3, v.minor);
    TEST_ASSERT_EQUAL_UINT32(456, v.patch);
    TEST_ASSERT_TRUE(v.has_pre);
    TEST_ASSERT_EQUAL_STRING("rc.10", v.pre);
    TEST_ASSERT_FALSE(parse_ok("1.0.0+meta").has_pre);
}

static void test_release_is_newer() {
    TEST_ASSERT_TRUE(release_is_newer("1.4.1-rc.3", "1.4.1-rc.10"));
    TEST_ASSERT_TRUE(release_is_newer("1.4.1-rc.10", "1.4.1"));
    TEST_ASSERT_TRUE(release_is_newer("1.4.1", "1.5.0-beta"));
    TEST_ASSERT_FALSE(release_is_newer("1.4.1", "1.4.1"));             // equal: up to date
    TEST_ASSERT_FALSE(release_is_newer("1.4.1+7", "1.4.1+9"));         // build metadata ignored
    TEST_ASSERT_FALSE(release_is_newer("1.5.0-rc.1", "1.4.0"));        // running an rc of the next version: no downgrade
    TEST_ASSERT_FALSE(release_is_newer("1.5.0", "1.5.0-rc.1"));
    TEST_ASSERT_FALSE(release_is_newer("garbage", "1.5.0"));
    TEST_ASSERT_FALSE(release_is_newer("1.5.0", "v1.6.0"));
    TEST_ASSERT_FALSE(release_is_newer(nullptr, "1.5.0"));
    TEST_ASSERT_FALSE(release_is_newer("1.5.0", nullptr));
}

//------------------------------------------------------------------------------
// Real output of tools/update_index.py against a fake firmware/ tree (releases v1.3.0 single-board,
// v1.4.0, v1.5.0-rc.2, v1.5.0-rc.10 unified, plus a tag "latest"; empty tree for the none document).
//------------------------------------------------------------------------------
static const char kGenStable[] =
    "{\"schema\":1,\"channel\":\"stable\",\"version\":\"1.4.0\",\"tag\":\"v1.4.0\",\"prerelease\":false,"
    "\"published\":\"2026-10-24\",\"size\":200000,"
    "\"sha256\":\"bef9446f87704d3d97e489c6343898c895f97f61a948f2ce970ab6331c35beed\","
    "\"url\":\"https://devnullgamer.github.io/smart-grind-by-weight/firmware/v1.4.0/smart-grind-by-weight-v1.4.0.bin\"}\n";
static const char kGenBeta[] =
    "{\"schema\":1,\"channel\":\"beta\",\"version\":\"1.5.0-rc.10\",\"tag\":\"v1.5.0-rc.10\",\"prerelease\":true,"
    "\"published\":\"2026-10-24\",\"size\":220000,"
    "\"sha256\":\"f51b650753d3b1689782a49905a2092bca9aec5db3b79b671a4ff840a42b65a2\","
    "\"url\":\"https://devnullgamer.github.io/smart-grind-by-weight/firmware/v1.5.0-rc.10/smart-grind-by-weight-v1.5.0-rc.10.bin\"}\n";
static const char kGenNone[] = "{\"schema\":1,\"channel\":\"stable\",\"none\":true}\n";

static ReleaseParseResult parse(const char* body, ReleaseInfo* out, const char* channel = "stable") {
    return release_parse_index(body, strlen(body), kSlot, channel, out);
}

static void test_parse_generator_stable() {
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, parse(kGenStable, &r));
    TEST_ASSERT_EQUAL_STRING("stable", r.channel);
    TEST_ASSERT_EQUAL_STRING("1.4.0", r.version);
    TEST_ASSERT_EQUAL_STRING("v1.4.0", r.tag);
    TEST_ASSERT_FALSE(r.prerelease);
    TEST_ASSERT_EQUAL_STRING("2026-10-24", r.published);
    TEST_ASSERT_EQUAL_UINT32(200000, r.size);
    TEST_ASSERT_EQUAL_STRING("bef9446f87704d3d97e489c6343898c895f97f61a948f2ce970ab6331c35beed", r.sha256);
    TEST_ASSERT_EQUAL_STRING(
        "https://devnullgamer.github.io/smart-grind-by-weight/firmware/v1.4.0/smart-grind-by-weight-v1.4.0.bin", r.url);
}

static void test_parse_generator_beta() {
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, parse(kGenBeta, &r, "beta"));
    TEST_ASSERT_EQUAL_STRING("beta", r.channel);
    TEST_ASSERT_EQUAL_STRING("1.5.0-rc.10", r.version);
    TEST_ASSERT_TRUE(r.prerelease);
    TEST_ASSERT_EQUAL_UINT32(220000, r.size);
    TEST_ASSERT_TRUE(release_is_newer("1.5.0-rc.2", r.version));
    TEST_ASSERT_FALSE(release_is_newer("1.5.0", r.version));
}

static void test_parse_generator_none() {
    ReleaseInfo r;
    r.size = 5;
    TEST_ASSERT_EQUAL(ReleaseParseResult::NONE, parse(kGenNone, &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.size);
    TEST_ASSERT_EQUAL(ReleaseParseResult::NONE, parse("{\"schema\":1,\"channel\":\"beta\",\"none\":true}", &r, "beta"));
}

// Builds a stable index from the generator fixture with one field changed: replaces `from` by `to`.
static ReleaseParseResult parse_mutated(const char* from, const char* to, ReleaseInfo* out = nullptr) {
    char body[UPDATE_PULL_INDEX_MAX_BYTES + 64];
    const char* at = strstr(kGenStable, from);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, from);
    const size_t head = static_cast<size_t>(at - kGenStable);
    int n = snprintf(body, sizeof(body), "%.*s%s%s", static_cast<int>(head), kGenStable, to, at + strlen(from));
    TEST_ASSERT_TRUE(n > 0 && n < static_cast<int>(sizeof(body)));
    ReleaseInfo tmp;
    return release_parse_index(body, static_cast<size_t>(n), kSlot, "stable", out ? out : &tmp);
}

static void test_parse_rejects_schema_and_channel() {
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SCHEMA, parse_mutated("\"schema\":1", "\"schema\":2"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SCHEMA, parse_mutated("\"schema\":1", "\"schema\":\"1\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SCHEMA, parse_mutated("\"schema\":1,", ""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse_mutated("\"stable\"", "\"nightly\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse_mutated("\"channel\":\"stable\",", ""));
}

static void test_parse_rejects_version_fields() {
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_VERSION, parse_mutated("\"version\":\"1.4.0\"", "\"version\":\"v1.4.0\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_VERSION, parse_mutated("\"version\":\"1.4.0\"", "\"version\":\"1.4\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_VERSION, parse_mutated("\"tag\":\"v1.4.0\"", "\"tag\":\"v1.4.1\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_VERSION, parse_mutated("\"tag\":\"v1.4.0\"", "\"tag\":\"1.4.0\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_VERSION, parse_mutated("\"prerelease\":false", "\"prerelease\":true"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MISSING_FIELD, parse_mutated("\"tag\":\"v1.4.0\",", ""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MISSING_FIELD, parse_mutated("\"prerelease\":false,", ""));
}

static void test_parse_rejects_published() {
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_PUBLISHED, parse_mutated("2026-10-24", "2026-1-24"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_PUBLISHED, parse_mutated("2026-10-24", "2026/10/24"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_PUBLISHED, parse_mutated("2026-10-24", "2026-10-24T00"));
}

static void test_parse_rejects_size() {
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SIZE, parse_mutated("\"size\":200000", "\"size\":\"200000\""));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SIZE, parse_mutated("\"size\":200000", "\"size\":2e5"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SIZE, parse_mutated("\"size\":200000", "\"size\":-5"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SIZE, parse_mutated("\"size\":200000", "\"size\":131071"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SIZE, parse_mutated("\"size\":200000", "\"size\":3145729"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SIZE, parse_mutated("\"size\":200000", "\"size\":99999999999"));
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, parse_mutated("\"size\":200000", "\"size\":131072", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, parse_mutated("\"size\":200000", "\"size\":3145728", &r));
}

static void test_parse_rejects_sha() {
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SHA, parse_mutated("bef9446f877", "bef9446f87"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SHA, parse_mutated("bef9446f877", "bef9446f87zz"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SHA, parse_mutated("bef9446f877", "bef9446f8777"));
}

static void test_parse_rejects_url() {
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_URL,
                      parse_mutated("https://devnullgamer.github.io/", "https://evil.example.com/"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_URL,
                      parse_mutated("https://devnullgamer.github.io/", "http://devnullgamer.github.io/"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_URL,
                      parse_mutated("https://devnullgamer.github.io/", "https://devnullgamer.github.io.evil.com/"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_URL, parse_mutated("v1.4.0.bin", "v1.4.0.bin.exe"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_URL, parse_mutated("smart-grind-by-weight/firmware", "smart-grind-by-weight/../x"));
}

static void test_parse_expected_channel() {
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse(kGenStable, &r, "beta"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse(kGenBeta, &r, "stable"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse(kGenNone, &r, "beta"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse(kGenStable, &r, "nightly"));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, release_parse_index(kGenStable, strlen(kGenStable), kSlot, nullptr, &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.size);
    // A prerelease inside a stable document is refused even when everything else is consistent
    const char* beta_at = strstr(kGenBeta, "\"beta\"");
    TEST_ASSERT_NOT_NULL(beta_at);
    char stable_pre[UPDATE_PULL_INDEX_MAX_BYTES];
    snprintf(stable_pre, sizeof(stable_pre), "%.*s\"stable\"%s", static_cast<int>(beta_at - kGenBeta), kGenBeta, beta_at + 6);
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_CHANNEL, parse(stable_pre, &r, "stable"));
}

static void test_parse_url_charset() {
    const char* const bad_tails[] = {"?x=1", "%41", "#f", ":80", "@h", " ", "~"};
    for (const char* tail : bad_tails) {
        char to[64];
        snprintf(to, sizeof(to), "v1.4.0.bin%s", tail);
        TEST_ASSERT_EQUAL_MESSAGE(ReleaseParseResult::BAD_URL, parse_mutated("v1.4.0.bin", to), tail);
    }
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, parse_mutated("firmware/v1.4.0/", "firmware/V_1-4.0/a/", &r));
}

static void test_parse_rejects_malformed() {
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::BODY_EMPTY, release_parse_index(nullptr, 0, kSlot, "stable", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BODY_EMPTY, release_parse_index("", 0, kSlot, "stable", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse("[]", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse("{\"schema\":1,}", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse("{\"schema\":1,\"schema\":1}", &r));  // duplicate key
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse_mutated("\"channel\":\"stable\",", "\"channel\":\"stable\",\"channel\":\"beta\","));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse_mutated("\"schema\":1,", "\"schema\":1,\"x\":{\"a\":1},"));  // nested
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse_mutated("\"stable\"", "\"sta\\u0062le\""));            // escape
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse_mutated("\"stable\"", "\"stable"));                    // unterminated
    // Trailing bytes and truncation
    char body[UPDATE_PULL_INDEX_MAX_BYTES];
    snprintf(body, sizeof(body), "%s x", kGenStable);
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse(body, &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, release_parse_index(kGenStable, strlen(kGenStable) / 2, kSlot, "stable", &r));
    // "none" must stand alone and be true
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse("{\"schema\":1,\"channel\":\"stable\",\"none\":false}", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, parse_mutated("\"schema\":1,", "\"schema\":1,\"none\":true,"));
    // A string longer than UPDATE_PULL_STRING_MAX
    char big[UPDATE_PULL_INDEX_MAX_BYTES];
    int n = snprintf(big, sizeof(big), "{\"schema\":1,\"channel\":\"stable\",\"x\":\"");
    for (int i = 0; i < UPDATE_PULL_STRING_MAX + 1; i++) big[n++] = 'a';
    n += snprintf(big + n, sizeof(big) - static_cast<size_t>(n), "\"}");
    TEST_ASSERT_EQUAL(ReleaseParseResult::MALFORMED, release_parse_index(big, static_cast<size_t>(n), kSlot, "stable", &r));
}

static void test_parse_rejects_oversized_body() {
    static char big[UPDATE_PULL_INDEX_MAX_BYTES + 2];
    memset(big, ' ', sizeof(big));
    memcpy(big, kGenStable, strlen(kGenStable));  // valid JSON padded with blanks beyond the limit
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::BODY_TOO_LARGE, release_parse_index(big, UPDATE_PULL_INDEX_MAX_BYTES + 1, kSlot, "stable", &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, release_parse_index(big, UPDATE_PULL_INDEX_MAX_BYTES, kSlot, "stable", &r));
}

static void test_parse_tolerates_unknown_flat_keys() {
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK,
                      parse_mutated("\"schema\":1,", "\"schema\":1,\"note\":\"hi\",\"n\":3,\"ok\":true,\"z\":null,", &r));
    TEST_ASSERT_EQUAL_STRING("1.4.0", r.version);
}

static void test_parse_failure_resets_output() {
    ReleaseInfo r;
    TEST_ASSERT_EQUAL(ReleaseParseResult::OK, parse(kGenStable, &r));
    TEST_ASSERT_EQUAL(ReleaseParseResult::BAD_SCHEMA, parse_mutated("\"schema\":1", "\"schema\":9", &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.size);
    TEST_ASSERT_EQUAL_CHAR('\0', r.version[0]);
}

static void test_result_text_covers_all() {
    for (int i = 0; i <= static_cast<int>(ReleaseParseResult::BAD_URL); i++) {
        const char* t = release_parse_result_text(static_cast<ReleaseParseResult>(i));
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(strlen(t) > 0);
        TEST_ASSERT_TRUE(strcmp(t, "bad index") != 0);
    }
}

void run_release_index_tests() {
    RUN_TEST(test_semver_ascending_order);
    RUN_TEST(test_semver_equal_ignores_build);
    RUN_TEST(test_semver_rejects_invalid);
    RUN_TEST(test_semver_fields);
    RUN_TEST(test_release_is_newer);
    RUN_TEST(test_parse_generator_stable);
    RUN_TEST(test_parse_generator_beta);
    RUN_TEST(test_parse_generator_none);
    RUN_TEST(test_parse_rejects_schema_and_channel);
    RUN_TEST(test_parse_rejects_version_fields);
    RUN_TEST(test_parse_rejects_published);
    RUN_TEST(test_parse_rejects_size);
    RUN_TEST(test_parse_rejects_sha);
    RUN_TEST(test_parse_rejects_url);
    RUN_TEST(test_parse_expected_channel);
    RUN_TEST(test_parse_url_charset);
    RUN_TEST(test_parse_rejects_malformed);
    RUN_TEST(test_parse_rejects_oversized_body);
    RUN_TEST(test_parse_tolerates_unknown_flat_keys);
    RUN_TEST(test_parse_failure_resets_output);
    RUN_TEST(test_result_text_covers_all);
}
