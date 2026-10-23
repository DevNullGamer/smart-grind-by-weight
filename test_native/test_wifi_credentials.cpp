#include <unity.h>
#include <string.h>
#include <vector>
#include "network/wifi_credentials.h"

static bool ssid_ok(const std::vector<uint8_t>& v) { return wifi_validate_ssid(v.data(), v.size()); }
static bool pass_ok(const std::vector<uint8_t>& v) { return wifi_validate_password(v.data(), v.size()); }

static std::vector<uint8_t> bytes(size_t n, uint8_t fill = 'a') { return std::vector<uint8_t>(n, fill); }

static const char* kToken = "0123456789abcdef";

static std::vector<uint8_t> token_bytes() { return std::vector<uint8_t>(kToken, kToken + 16); }

// Builds [ssid_len][ssid][pass_len][pass][token16] with explicit length bytes
static std::vector<uint8_t> payload(const std::vector<uint8_t>& ssid, const std::vector<uint8_t>& pass,
                                    const std::vector<uint8_t>& token = token_bytes()) {
    std::vector<uint8_t> p;
    p.push_back(static_cast<uint8_t>(ssid.size()));
    p.insert(p.end(), ssid.begin(), ssid.end());
    p.push_back(static_cast<uint8_t>(pass.size()));
    p.insert(p.end(), pass.begin(), pass.end());
    p.insert(p.end(), token.begin(), token.end());
    return p;
}

static WifiProvisionResult parse(const std::vector<uint8_t>& p, WifiProvisionFields* f) {
    return wifi_parse_provisioning(p.data(), p.size(), f);
}

void test_ssid_length_boundaries() {
    TEST_ASSERT_FALSE(ssid_ok(bytes(0)));
    TEST_ASSERT_TRUE(ssid_ok(bytes(1)));
    TEST_ASSERT_TRUE(ssid_ok(bytes(32)));
    TEST_ASSERT_FALSE(ssid_ok(bytes(33)));
    TEST_ASSERT_FALSE(ssid_ok(bytes(255)));
}

void test_ssid_rejects_nul_anywhere() {
    for (size_t pos : {0u, 5u, 31u}) {
        std::vector<uint8_t> s = bytes(32);
        s[pos] = 0;
        TEST_ASSERT_FALSE(ssid_ok(s));
    }
}

void test_ssid_null_pointer() {
    TEST_ASSERT_FALSE(wifi_validate_ssid(nullptr, 5));
}

void test_ssid_accepts_utf8_and_spaces() {
    const uint8_t s[] = {'C', 'a', 'f', 0xC3, 0xA9, ' ', '5', 'G'};
    TEST_ASSERT_TRUE(wifi_validate_ssid(s, sizeof(s)));
}

void test_password_empty_is_open_network() {
    TEST_ASSERT_TRUE(pass_ok(bytes(0)));
    TEST_ASSERT_TRUE(wifi_validate_password(nullptr, 0));
}

void test_password_length_boundaries() {
    TEST_ASSERT_FALSE(pass_ok(bytes(1)));
    TEST_ASSERT_FALSE(pass_ok(bytes(7)));
    TEST_ASSERT_TRUE(pass_ok(bytes(8)));
    TEST_ASSERT_TRUE(pass_ok(bytes(63)));
    TEST_ASSERT_FALSE(pass_ok(bytes(64)));
    TEST_ASSERT_FALSE(pass_ok(bytes(255)));
}

void test_password_rejects_nul() {
    std::vector<uint8_t> p = bytes(20);
    p[10] = 0;
    TEST_ASSERT_FALSE(pass_ok(p));
    p = bytes(8);
    p[7] = 0;
    TEST_ASSERT_FALSE(pass_ok(p));
}

void test_password_null_pointer_with_length() {
    TEST_ASSERT_FALSE(wifi_validate_password(nullptr, 8));
}

void test_hostname_validation() {
    TEST_ASSERT_TRUE(wifi_validate_hostname("grindbyweight"));
    TEST_ASSERT_TRUE(wifi_validate_hostname("a"));
    TEST_ASSERT_TRUE(wifi_validate_hostname("grinder-2"));
    TEST_ASSERT_FALSE(wifi_validate_hostname(""));
    TEST_ASSERT_FALSE(wifi_validate_hostname(nullptr));
    TEST_ASSERT_FALSE(wifi_validate_hostname("-grinder"));
    TEST_ASSERT_FALSE(wifi_validate_hostname("grinder-"));
    TEST_ASSERT_FALSE(wifi_validate_hostname("Grinder"));
    TEST_ASSERT_FALSE(wifi_validate_hostname("grind er"));
    TEST_ASSERT_FALSE(wifi_validate_hostname("grinder.local"));
    TEST_ASSERT_FALSE(wifi_validate_hostname("grinder_1"));
}

void test_hostname_length_boundaries() {
    char name[80];
    memset(name, 'a', sizeof(name));
    name[63] = '\0';
    TEST_ASSERT_TRUE(wifi_validate_hostname(name));
    name[63] = 'a';
    name[64] = '\0';
    TEST_ASSERT_FALSE(wifi_validate_hostname(name));
}

void test_token_valid() {
    TEST_ASSERT_TRUE(wifi_token_is_valid("0123456789abcdef"));
    TEST_ASSERT_TRUE(wifi_token_is_valid("0000000000000000"));
}

void test_token_invalid() {
    TEST_ASSERT_FALSE(wifi_token_is_valid(nullptr));
    TEST_ASSERT_FALSE(wifi_token_is_valid(""));
    TEST_ASSERT_FALSE(wifi_token_is_valid("0123456789abcde"));    // 15
    TEST_ASSERT_FALSE(wifi_token_is_valid("0123456789abcdef0"));  // 17
    TEST_ASSERT_FALSE(wifi_token_is_valid("0123456789ABCDEF"));   // uppercase
    TEST_ASSERT_FALSE(wifi_token_is_valid("0123456789abcdeg"));
    TEST_ASSERT_FALSE(wifi_token_is_valid("0123456789abcde "));
}

void test_token_bytes_validation() {
    TEST_ASSERT_TRUE(wifi_validate_token_bytes(token_bytes().data(), 16));
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(token_bytes().data(), 15));
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(token_bytes().data(), 17));
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(token_bytes().data(), 0));
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(nullptr, 16));
    std::vector<uint8_t> t = token_bytes();
    t[3] = 'A';
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(t.data(), 16));
    t = token_bytes();
    t[15] = 'g';
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(t.data(), 16));
    t = token_bytes();
    t[8] = 0;
    TEST_ASSERT_FALSE(wifi_validate_token_bytes(t.data(), 16));
}

void test_token_equal() {
    TEST_ASSERT_TRUE(wifi_token_equal("0123456789abcdef", "0123456789abcdef"));
    TEST_ASSERT_FALSE(wifi_token_equal("0123456789abcdef", "0123456789abcdee"));
    TEST_ASSERT_FALSE(wifi_token_equal("1123456789abcdef", "0123456789abcdef"));
    TEST_ASSERT_FALSE(wifi_token_equal("0123456789abcdef", "0123456789abcde"));
    TEST_ASSERT_FALSE(wifi_token_equal("", ""));
    TEST_ASSERT_FALSE(wifi_token_equal(nullptr, "0123456789abcdef"));
    TEST_ASSERT_FALSE(wifi_token_equal("0123456789ABCDEF", "0123456789ABCDEF"));
}

void test_provisioning_allowed() {
    TEST_ASSERT_TRUE(wifi_provisioning_allowed(false, false));
    TEST_ASSERT_TRUE(wifi_provisioning_allowed(false, true));
    TEST_ASSERT_TRUE(wifi_provisioning_allowed(true, true));
    TEST_ASSERT_FALSE(wifi_provisioning_allowed(true, false));
}

void test_auth_failure_reasons() {
    TEST_ASSERT_TRUE(wifi_disconnect_is_auth_failure(2));
    TEST_ASSERT_TRUE(wifi_disconnect_is_auth_failure(15));
    TEST_ASSERT_TRUE(wifi_disconnect_is_auth_failure(202));
    TEST_ASSERT_FALSE(wifi_disconnect_is_auth_failure(201));  // no AP found
    TEST_ASSERT_FALSE(wifi_disconnect_is_auth_failure(200));  // beacon timeout
    TEST_ASSERT_FALSE(wifi_disconnect_is_auth_failure(0));
}

void test_backoff_sequence() {
    uint32_t b = 0;
    const uint32_t expected[] = {5000, 10000, 20000, 40000, 60000, 60000, 60000};
    for (uint32_t e : expected) {
        b = wifi_backoff_next(b);
        TEST_ASSERT_EQUAL_UINT32(e, b);
    }
}

void test_backoff_edges() {
    TEST_ASSERT_EQUAL_UINT32(5000, wifi_backoff_next(1));
    TEST_ASSERT_EQUAL_UINT32(5000, wifi_backoff_next(4999));
    TEST_ASSERT_EQUAL_UINT32(10000, wifi_backoff_next(5000));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_backoff_next(30000));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_backoff_next(29999 + 1));
    TEST_ASSERT_EQUAL_UINT32(59998, wifi_backoff_next(29999));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_backoff_next(60000));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_backoff_next(UINT32_MAX));
}

void test_parse_ok() {
    WifiProvisionFields f;
    const std::vector<uint8_t> ssid = {'h', 'o', 'm', 'e'};
    const std::vector<uint8_t> pass = {'s', 'e', 'c', 'r', 'e', 't', '1', '2', '3'};
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::OK), static_cast<int>(parse(payload(ssid, pass), &f)));
    TEST_ASSERT_EQUAL_STRING("home", f.ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", f.password);
}

void test_parse_open_network() {
    WifiProvisionFields f;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::OK), static_cast<int>(parse(payload(bytes(4), bytes(0)), &f)));
    TEST_ASSERT_EQUAL_STRING("aaaa", f.ssid);
    TEST_ASSERT_EQUAL_STRING("", f.password);
}

void test_parse_max_lengths() {
    WifiProvisionFields f;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::OK), static_cast<int>(parse(payload(bytes(32, 's'), bytes(63, 'p')), &f)));
    TEST_ASSERT_EQUAL_UINT(32, strlen(f.ssid));
    TEST_ASSERT_EQUAL_UINT(63, strlen(f.password));
}

void test_parse_ssid_length_violations() {
    WifiProvisionFields f;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_SSID), static_cast<int>(parse(payload(bytes(0), bytes(8)), &f)));
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_SSID), static_cast<int>(parse(payload(bytes(33), bytes(8)), &f)));
    // Oversize length byte with a payload that is far too short
    const uint8_t huge[] = {255, 'a'};
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_SSID), static_cast<int>(wifi_parse_provisioning(huge, sizeof(huge), &f)));
}

void test_parse_password_length_violations() {
    WifiProvisionFields f;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_PASSWORD), static_cast<int>(parse(payload(bytes(4), bytes(7)), &f)));
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_PASSWORD), static_cast<int>(parse(payload(bytes(4), bytes(64)), &f)));
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_PASSWORD), static_cast<int>(parse(payload(bytes(4), bytes(255)), &f)));
}

void test_parse_nul_in_fields() {
    WifiProvisionFields f;
    std::vector<uint8_t> ssid = bytes(6);
    ssid[3] = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_SSID), static_cast<int>(parse(payload(ssid, bytes(8)), &f)));
    std::vector<uint8_t> pass = bytes(10);
    pass[9] = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_PASSWORD), static_cast<int>(parse(payload(bytes(6), pass), &f)));
}

void test_parse_truncated() {
    WifiProvisionFields f;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRUNCATED), static_cast<int>(wifi_parse_provisioning(nullptr, 0, &f)));
    const uint8_t empty[] = {0};
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRUNCATED), static_cast<int>(wifi_parse_provisioning(empty, 0, &f)));
    // Every proper prefix of a valid payload is rejected
    const std::vector<uint8_t> full = payload(bytes(5, 's'), bytes(9, 'p'));
    for (size_t n = 0; n < full.size(); n++) {
        WifiProvisionResult r = wifi_parse_provisioning(full.data(), n, &f);
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(WifiProvisionResult::OK), static_cast<int>(r));
    }
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::OK), static_cast<int>(wifi_parse_provisioning(full.data(), full.size(), &f)));
}

void test_parse_missing_password_length_byte() {
    WifiProvisionFields f;
    const uint8_t only_ssid[] = {3, 'a', 'b', 'c'};
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRUNCATED), static_cast<int>(wifi_parse_provisioning(only_ssid, sizeof(only_ssid), &f)));
}

void test_parse_password_shorter_than_announced() {
    WifiProvisionFields f;
    // Announces 10 password bytes but only 9 follow, and no token
    std::vector<uint8_t> p = payload(bytes(4), bytes(10), std::vector<uint8_t>());
    p.pop_back();
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRUNCATED), static_cast<int>(parse(p, &f)));
}

void test_parse_token_lengths() {
    WifiProvisionFields f;
    for (size_t n : {0u, 15u}) {
        TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRUNCATED),
                          static_cast<int>(parse(payload(bytes(4), bytes(8), bytes(n, '0')), &f)));
    }
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::OK),
                      static_cast<int>(parse(payload(bytes(4), bytes(8), bytes(16, '0')), &f)));
    TEST_ASSERT_EQUAL_STRING("0000000000000000", f.token);
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRAILING_BYTES),
                      static_cast<int>(parse(payload(bytes(4), bytes(8), bytes(17, '0')), &f)));
}

void test_parse_bad_token_bytes() {
    WifiProvisionFields f;
    std::vector<uint8_t> t = token_bytes();
    t[0] = 'A';
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_TOKEN), static_cast<int>(parse(payload(bytes(4), bytes(8), t), &f)));
    t = token_bytes();
    t[7] = 'z';
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_TOKEN), static_cast<int>(parse(payload(bytes(4), bytes(8), t), &f)));
    t = token_bytes();
    t[12] = 0;
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::BAD_TOKEN), static_cast<int>(parse(payload(bytes(4), bytes(8), t), &f)));
}

void test_parse_max_frame_is_114_bytes() {
    // command byte + max SSID + max password + token
    TEST_ASSERT_EQUAL_UINT(114 - 1, payload(bytes(32), bytes(63)).size());
}

void test_parse_trailing_bytes() {
    WifiProvisionFields f;
    std::vector<uint8_t> p = payload(bytes(4), bytes(8));
    p.push_back('x');
    TEST_ASSERT_EQUAL(static_cast<int>(WifiProvisionResult::TRAILING_BYTES), static_cast<int>(parse(p, &f)));
}

void test_parse_failure_leaves_fields_zeroed() {
    WifiProvisionFields f;
    memset(&f, 0x41, sizeof(f));
    std::vector<uint8_t> p = payload(bytes(4), bytes(8));
    p.push_back('x');
    parse(p, &f);
    TEST_ASSERT_EQUAL_UINT8(0, f.ssid[0]);
    TEST_ASSERT_EQUAL_UINT8(0, f.password[0]);
    TEST_ASSERT_EQUAL_UINT8(0, f.token[0]);
}

void run_wifi_credentials_tests() {
    RUN_TEST(test_ssid_length_boundaries);
    RUN_TEST(test_ssid_rejects_nul_anywhere);
    RUN_TEST(test_ssid_null_pointer);
    RUN_TEST(test_ssid_accepts_utf8_and_spaces);
    RUN_TEST(test_password_empty_is_open_network);
    RUN_TEST(test_password_length_boundaries);
    RUN_TEST(test_password_rejects_nul);
    RUN_TEST(test_password_null_pointer_with_length);
    RUN_TEST(test_hostname_validation);
    RUN_TEST(test_hostname_length_boundaries);
    RUN_TEST(test_token_valid);
    RUN_TEST(test_token_invalid);
    RUN_TEST(test_token_bytes_validation);
    RUN_TEST(test_token_equal);
    RUN_TEST(test_provisioning_allowed);
    RUN_TEST(test_auth_failure_reasons);
    RUN_TEST(test_backoff_sequence);
    RUN_TEST(test_backoff_edges);
    RUN_TEST(test_parse_ok);
    RUN_TEST(test_parse_open_network);
    RUN_TEST(test_parse_max_lengths);
    RUN_TEST(test_parse_ssid_length_violations);
    RUN_TEST(test_parse_password_length_violations);
    RUN_TEST(test_parse_nul_in_fields);
    RUN_TEST(test_parse_truncated);
    RUN_TEST(test_parse_missing_password_length_byte);
    RUN_TEST(test_parse_password_shorter_than_announced);
    RUN_TEST(test_parse_token_lengths);
    RUN_TEST(test_parse_bad_token_bytes);
    RUN_TEST(test_parse_max_frame_is_114_bytes);
    RUN_TEST(test_parse_trailing_bytes);
    RUN_TEST(test_parse_failure_leaves_fields_zeroed);
}
