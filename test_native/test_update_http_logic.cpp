#include <unity.h>
#include <string.h>
#include <string>
#include "network/update_http_logic.h"

static const uint32_t kSlot = 3072u * 1024u;
static const char* kSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static BeginParseResult parse(const std::string& body, UpdateBeginRequest* out, uint32_t slot = kSlot) {
    return http_parse_begin_request(body.data(), body.size(), slot, out);
}

static std::string good_body(const std::string& extra = "") {
    return std::string("{\"size\":2000000,\"sha256\":\"") + kSha + "\"" + extra + "}";
}

// ---- bearer ----

static void test_bearer_valid() {
    char t[WIFI_TOKEN_LEN + 1];
    TEST_ASSERT_TRUE(http_parse_bearer("Bearer 0123456789abcdef", t));
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", t);
    TEST_ASSERT_TRUE(http_parse_bearer("bearer 0123456789abcdef", t));
    TEST_ASSERT_TRUE(http_parse_bearer("BEARER fedcba9876543210", t));
}

static void test_bearer_invalid() {
    char t[WIFI_TOKEN_LEN + 1];
    TEST_ASSERT_FALSE(http_parse_bearer(nullptr, t));
    TEST_ASSERT_FALSE(http_parse_bearer("", t));
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer", t));
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer ", t));
    TEST_ASSERT_FALSE(http_parse_bearer("Basic 0123456789abcdef", t));
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer  0123456789abcdef", t));      // two spaces
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer 0123456789abcde", t));        // 15
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer 0123456789abcdef0", t));      // 17
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer 0123456789abcdef ", t));      // trailing space
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer 0123456789ABCDEF", t));       // uppercase hex
    TEST_ASSERT_FALSE(http_parse_bearer("Bearer 0123456789abcdeg", t));
    TEST_ASSERT_FALSE(http_parse_bearer("0123456789abcdef", t));
    TEST_ASSERT_EQUAL_CHAR('\0', t[0]);  // cleared on failure
}

// ---- lockout ----

static void test_lockout_three_failures_lock_60s() {
    AuthLockout l;
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 1000));
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 1001));
    TEST_ASSERT_FALSE(auth_lockout_active(&l, 1002));
    TEST_ASSERT_TRUE(auth_lockout_record_failure(&l, 1002));
    TEST_ASSERT_TRUE(auth_lockout_active(&l, 1003));
    TEST_ASSERT_EQUAL_UINT32(59999, auth_lockout_remaining_ms(&l, 1003));
    TEST_ASSERT_TRUE(auth_lockout_active(&l, 1002 + 59999));
    TEST_ASSERT_FALSE(auth_lockout_active(&l, 1002 + 60000));
    TEST_ASSERT_EQUAL_UINT32(0, auth_lockout_remaining_ms(&l, 1002 + 60000));
}

static void test_lockout_doubles_and_caps() {
    AuthLockout l;
    uint32_t now = 0;
    uint32_t expected = 60000;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 2; i++) TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, now));
        TEST_ASSERT_TRUE(auth_lockout_record_failure(&l, now));
        TEST_ASSERT_EQUAL_UINT32(expected, auth_lockout_remaining_ms(&l, now));
        now += expected;
        TEST_ASSERT_FALSE(auth_lockout_active(&l, now));
        expected = expected * 2 > 900000 ? 900000 : expected * 2;
    }
    TEST_ASSERT_EQUAL_UINT32(900000, expected);
}

static void test_lockout_success_resets() {
    AuthLockout l;
    auth_lockout_record_failure(&l, 0);
    auth_lockout_record_failure(&l, 0);
    auth_lockout_record_success(&l);
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 0));  // counting restarted
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 0));
    TEST_ASSERT_TRUE(auth_lockout_record_failure(&l, 0));
    auth_lockout_active(&l, 70000);                        // expire the first lock
    auth_lockout_record_success(&l);
    for (int i = 0; i < 2; i++) auth_lockout_record_failure(&l, 70000);
    TEST_ASSERT_TRUE(auth_lockout_record_failure(&l, 70000));
    TEST_ASSERT_EQUAL_UINT32(60000, auth_lockout_remaining_ms(&l, 70000));  // duration back to the initial value
}

static void test_lockout_failures_while_locked_ignored() {
    AuthLockout l;
    for (int i = 0; i < 3; i++) auth_lockout_record_failure(&l, 0);
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 10));
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 20));
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 30));
    TEST_ASSERT_FALSE(auth_lockout_active(&l, 60000));
    // After expiry the counter starts from zero: three more failures are needed for the next (doubled) lock
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 60000));
    TEST_ASSERT_FALSE(auth_lockout_record_failure(&l, 60000));
    TEST_ASSERT_TRUE(auth_lockout_record_failure(&l, 60000));
    TEST_ASSERT_EQUAL_UINT32(120000, auth_lockout_remaining_ms(&l, 60000));
}

static void test_lockout_millis_wrap() {
    AuthLockout l;
    const uint32_t start = 0xFFFFFFFFu - 10000u;  // lock spans the 32-bit wrap
    for (int i = 0; i < 3; i++) auth_lockout_record_failure(&l, start);
    TEST_ASSERT_TRUE(auth_lockout_active(&l, start + 5000u));
    TEST_ASSERT_TRUE(auth_lockout_active(&l, start + 20000u));  // already wrapped past zero
    TEST_ASSERT_EQUAL_UINT32(40000, auth_lockout_remaining_ms(&l, start + 20000u));
    TEST_ASSERT_FALSE(auth_lockout_active(&l, start + 60001u));
}

static void test_lockout_expired_long_ago_is_not_active_again() {
    AuthLockout l;
    for (int i = 0; i < 3; i++) auth_lockout_record_failure(&l, 1000);
    // Consulted for the first time after more than half the millis() range: must read as expired
    TEST_ASSERT_FALSE(auth_lockout_active(&l, 1000u + 0x90000000u));
    AuthLockout m;
    for (int i = 0; i < 3; i++) auth_lockout_record_failure(&m, 1000);
    TEST_ASSERT_FALSE(auth_lockout_active(&m, 1000u + 60000u + 0x7FFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0, auth_lockout_remaining_ms(&m, 1000u + 60000u + 0x7FFFFFFFu));
}

// ---- begin request ----

static void test_begin_good_minimal() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(), &r));
    TEST_ASSERT_EQUAL_UINT32(2000000, r.size);
    TEST_ASSERT_EQUAL_STRING(kSha, r.sha256);
    TEST_ASSERT_EQUAL_INT32(-1, r.build);
    TEST_ASSERT_EQUAL_CHAR('\0', r.version[0]);
}

static void test_begin_good_full_and_whitespace() {
    UpdateBeginRequest r;
    const std::string body = std::string(" {\n \"version\" : \"1.2.3-beta\" ,\r\n\"build\":  207 , \"sha256\" : \"") + kSha +
                             "\" , \"size\" : 2191088 } \n";
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(body, &r));
    TEST_ASSERT_EQUAL_UINT32(2191088, r.size);
    TEST_ASSERT_EQUAL_INT32(207, r.build);
    TEST_ASSERT_EQUAL_STRING("1.2.3-beta", r.version);
}

static void test_begin_uppercase_sha_accepted() {
    UpdateBeginRequest r;
    std::string up = kSha;
    for (auto& c : up) if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 32);
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse("{\"size\":2000000,\"sha256\":\"" + up + "\"}", &r));
}

static void test_begin_size_limits() {
    UpdateBeginRequest r;
    auto with_size = [&](const char* n) {
        return parse(std::string("{\"size\":") + n + ",\"sha256\":\"" + kSha + "\"}", &r);
    };
    TEST_ASSERT_EQUAL(BeginParseResult::SIZE_TOO_SMALL, with_size("0"));
    TEST_ASSERT_EQUAL(BeginParseResult::SIZE_TOO_SMALL, with_size("131071"));
    TEST_ASSERT_EQUAL(BeginParseResult::OK, with_size("131072"));
    TEST_ASSERT_EQUAL(BeginParseResult::OK, with_size("3145728"));
    TEST_ASSERT_EQUAL(BeginParseResult::SIZE_TOO_LARGE, with_size("3145729"));
    TEST_ASSERT_EQUAL(BeginParseResult::SIZE_TOO_LARGE, with_size("4294967295"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("4294967296"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("99999999999"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("-5"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("2e6"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("2000000.5"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("0200000"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("\"2000000\""));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("true"));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SIZE, with_size("null"));
}

static void test_begin_slot_size_is_the_upper_bound() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(), &r, 2000000));
    TEST_ASSERT_EQUAL(BeginParseResult::SIZE_TOO_LARGE, parse(good_body(), &r, 1999999));
}

static void test_begin_missing_and_bad_sha() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::MISSING_SHA, parse("{\"size\":2000000}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MISSING_SIZE, parse(std::string("{\"sha256\":\"") + kSha + "\"}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MISSING_SIZE, parse("{}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SHA, parse("{\"size\":2000000,\"sha256\":\"abcd\"}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SHA, parse("{\"size\":2000000,\"sha256\":12}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SHA,
                      parse(std::string("{\"size\":2000000,\"sha256\":\"") + kSha + "0\"}", &r));
    std::string bad = kSha;
    bad[5] = 'z';
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SHA, parse("{\"size\":2000000,\"sha256\":\"" + bad + "\"}", &r));
}

static void test_begin_build_rules() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"build\":0"), &r));
    TEST_ASSERT_EQUAL_INT32(0, r.build);
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"build\":2147483647"), &r));
    TEST_ASSERT_EQUAL_INT32(2147483647, r.build);
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_BUILD, parse(good_body(",\"build\":2147483648"), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_BUILD, parse(good_body(",\"build\":-1"), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_BUILD, parse(good_body(",\"build\":\"7\""), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_BUILD, parse(good_body(",\"build\":1.5"), &r));
}

static void test_begin_version_rules() {
    UpdateBeginRequest r;
    const std::string v32(32, 'v');
    const std::string v33(33, 'v');
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"version\":\"" + v32 + "\""), &r));
    TEST_ASSERT_EQUAL_STRING(v32.c_str(), r.version);
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_VERSION, parse(good_body(",\"version\":\"" + v33 + "\""), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_VERSION, parse(good_body(",\"version\":5"), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"version\":\"\""), &r));
    TEST_ASSERT_EQUAL_CHAR('\0', r.version[0]);
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"version\":\"a\\\"b\""), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"version\":\"a\tb\""), &r));
}

static void test_begin_unknown_fields() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"note\":\"hello\",\"x\":12,\"y\":true,\"z\":null,\"w\":false"), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"obj\":{\"a\":1}"), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"arr\":[1]"), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"s\":\"" + std::string(64, 'a') + "\""), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"s\":\"" + std::string(65, 'a') + "\""), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"" + std::string(33, 'k') + "\":1"), &r));
}

static void test_begin_malformed() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::BODY_EMPTY, http_parse_begin_request(nullptr, 0, kSlot, &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BODY_EMPTY, parse("", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("   ", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("[]", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{\"size\"", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{\"size\":", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{\"x\":1,}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{\"x\":1 \"y\":2}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{x:1}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse("{\"\":1}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body() + " x", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body() + "{}", &r));
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"size\":2000000"), &r));  // duplicate key
    TEST_ASSERT_EQUAL(BeginParseResult::MALFORMED, parse(good_body(",\"x\":tru"), &r));
}

static void test_begin_body_size_limit() {
    UpdateBeginRequest r;
    std::string body = good_body();
    TEST_ASSERT_TRUE(body.size() < UPDATE_BEGIN_BODY_MAX);
    std::string ok = body + std::string(UPDATE_BEGIN_BODY_MAX - body.size(), ' ');
    TEST_ASSERT_EQUAL_size_t(UPDATE_BEGIN_BODY_MAX, ok.size());
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(ok, &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BODY_TOO_LARGE, parse(ok + " ", &r));
}

static void test_begin_failure_resets_output() {
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::OK, parse(good_body(",\"build\":5,\"version\":\"x\""), &r));
    TEST_ASSERT_EQUAL(BeginParseResult::BAD_SHA, parse("{\"size\":2000000,\"sha256\":\"zz\"}", &r));
    TEST_ASSERT_EQUAL_UINT32(0, r.size);
    TEST_ASSERT_EQUAL_INT32(-1, r.build);
    TEST_ASSERT_EQUAL_CHAR('\0', r.sha256[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', r.version[0]);
}

static void test_begin_body_not_nul_terminated() {
    // The parser must respect len: a longer buffer with valid JSON after len is not read
    const std::string body = good_body();
    std::string with_tail = body + "garbage";
    UpdateBeginRequest r;
    TEST_ASSERT_EQUAL(BeginParseResult::OK, http_parse_begin_request(with_tail.data(), body.size(), kSlot, &r));
}

// ---- session id ----

static void test_session_id() {
    char id[UPDATE_SESSION_ID_LEN + 1];
    session_id_from_random(0x0000ABCDu, id);
    TEST_ASSERT_EQUAL_STRING("0000abcd", id);
    session_id_from_random(0xFFFFFFFFu, id);
    TEST_ASSERT_EQUAL_STRING("ffffffff", id);
    TEST_ASSERT_TRUE(session_id_valid(id));
    TEST_ASSERT_FALSE(session_id_valid(nullptr));
    TEST_ASSERT_FALSE(session_id_valid("abc"));
    TEST_ASSERT_FALSE(session_id_valid("abcdef012"));
    TEST_ASSERT_FALSE(session_id_valid("ABCDEF01"));
    TEST_ASSERT_FALSE(session_id_valid("abcdefg1"));
}

static void test_session_id_equal() {
    TEST_ASSERT_TRUE(session_id_equal("0123abcd", "0123abcd"));
    TEST_ASSERT_FALSE(session_id_equal("0123abcd", "0123abce"));
    TEST_ASSERT_FALSE(session_id_equal("0123abcd", "1123abcd"));
    TEST_ASSERT_FALSE(session_id_equal("0123abcd", nullptr));
    TEST_ASSERT_FALSE(session_id_equal("", ""));        // invalid ids never match each other
    TEST_ASSERT_FALSE(session_id_equal("xxxxxxxx", "xxxxxxxx"));
}

// ---- upload length ----

static void test_upload_length() {
    TEST_ASSERT_EQUAL(UploadLengthResult::OK, upload_length_check(2000000, 2000000));
    TEST_ASSERT_EQUAL(UploadLengthResult::MISSING, upload_length_check(0, 2000000));
    TEST_ASSERT_EQUAL(UploadLengthResult::MISMATCH, upload_length_check(1999999, 2000000));
    TEST_ASSERT_EQUAL(UploadLengthResult::MISMATCH, upload_length_check(2000001, 2000000));
}

void run_update_http_logic_tests() {
    RUN_TEST(test_bearer_valid);
    RUN_TEST(test_bearer_invalid);
    RUN_TEST(test_lockout_three_failures_lock_60s);
    RUN_TEST(test_lockout_doubles_and_caps);
    RUN_TEST(test_lockout_success_resets);
    RUN_TEST(test_lockout_failures_while_locked_ignored);
    RUN_TEST(test_lockout_millis_wrap);
    RUN_TEST(test_lockout_expired_long_ago_is_not_active_again);
    RUN_TEST(test_begin_good_minimal);
    RUN_TEST(test_begin_good_full_and_whitespace);
    RUN_TEST(test_begin_uppercase_sha_accepted);
    RUN_TEST(test_begin_size_limits);
    RUN_TEST(test_begin_slot_size_is_the_upper_bound);
    RUN_TEST(test_begin_missing_and_bad_sha);
    RUN_TEST(test_begin_build_rules);
    RUN_TEST(test_begin_version_rules);
    RUN_TEST(test_begin_unknown_fields);
    RUN_TEST(test_begin_malformed);
    RUN_TEST(test_begin_body_size_limit);
    RUN_TEST(test_begin_failure_resets_output);
    RUN_TEST(test_begin_body_not_nul_terminated);
    RUN_TEST(test_session_id);
    RUN_TEST(test_session_id_equal);
    RUN_TEST(test_upload_length);
}
