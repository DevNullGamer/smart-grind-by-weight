#include <unity.h>
#include <string.h>
#include "system/sha_hex.h"

// SHA-256 of "abc"
static const char* kAbcHex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static void test_sha_hex_parse_valid() {
    uint8_t out[SHA256_DIGEST_LEN];
    TEST_ASSERT_TRUE(sha256_hex_parse(kAbcHex, out));
    TEST_ASSERT_EQUAL_UINT8(0xba, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x78, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0xad, out[31]);
}

static void test_sha_hex_parse_uppercase() {
    char upper[SHA256_HEX_LEN + 1];
    for (size_t i = 0; i <= SHA256_HEX_LEN; i++) upper[i] = (kAbcHex[i] >= 'a' && kAbcHex[i] <= 'f') ? kAbcHex[i] - 32 : kAbcHex[i];
    uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];
    TEST_ASSERT_TRUE(sha256_hex_parse(upper, a));
    TEST_ASSERT_TRUE(sha256_hex_parse(kAbcHex, b));
    TEST_ASSERT_TRUE(sha256_equal(a, b));
}

static void test_sha_hex_parse_rejects_bad_input() {
    uint8_t out[SHA256_DIGEST_LEN];
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_FALSE(sha256_hex_parse(nullptr, out));
    TEST_ASSERT_FALSE(sha256_hex_parse("", out));
    TEST_ASSERT_FALSE(sha256_hex_parse("ba78", out));
    char shorter[SHA256_HEX_LEN];  // 63 characters
    memcpy(shorter, kAbcHex, SHA256_HEX_LEN - 1);
    shorter[SHA256_HEX_LEN - 1] = '\0';
    TEST_ASSERT_FALSE(sha256_hex_parse(shorter, out));
    char longer[SHA256_HEX_LEN + 2];  // 65 characters
    memcpy(longer, kAbcHex, SHA256_HEX_LEN);
    longer[SHA256_HEX_LEN] = '0';
    longer[SHA256_HEX_LEN + 1] = '\0';
    TEST_ASSERT_FALSE(sha256_hex_parse(longer, out));
    char bad[SHA256_HEX_LEN + 1];
    memcpy(bad, kAbcHex, sizeof(bad));
    bad[10] = 'g';
    TEST_ASSERT_FALSE(sha256_hex_parse(bad, out));
    bad[10] = ' ';
    TEST_ASSERT_FALSE(sha256_hex_parse(bad, out));
    // The output is untouched after every failure
    for (size_t i = 0; i < sizeof(out); i++) TEST_ASSERT_EQUAL_UINT8(0x5A, out[i]);
}

static void test_sha_hex_roundtrip() {
    uint8_t d[SHA256_DIGEST_LEN];
    char hex[SHA256_HEX_LEN + 1];
    TEST_ASSERT_TRUE(sha256_hex_parse(kAbcHex, d));
    sha256_to_hex(d, hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING(kAbcHex, hex);
}

static void test_sha_to_hex_small_buffer() {
    uint8_t d[SHA256_DIGEST_LEN] = {0};
    char hex[SHA256_HEX_LEN];  // one byte too small for the NUL
    memset(hex, 'x', sizeof(hex));
    sha256_to_hex(d, hex, sizeof(hex));
    TEST_ASSERT_EQUAL_CHAR('\0', hex[0]);
}

static void test_sha_equal_detects_single_bit() {
    uint8_t a[SHA256_DIGEST_LEN], b[SHA256_DIGEST_LEN];
    TEST_ASSERT_TRUE(sha256_hex_parse(kAbcHex, a));
    memcpy(b, a, sizeof(a));
    TEST_ASSERT_TRUE(sha256_equal(a, b));
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) {
        b[i] ^= 0x01;
        TEST_ASSERT_FALSE(sha256_equal(a, b));
        b[i] ^= 0x01;
    }
}

void run_sha_hex_tests() {
    RUN_TEST(test_sha_hex_parse_valid);
    RUN_TEST(test_sha_hex_parse_uppercase);
    RUN_TEST(test_sha_hex_parse_rejects_bad_input);
    RUN_TEST(test_sha_hex_roundtrip);
    RUN_TEST(test_sha_to_hex_small_buffer);
    RUN_TEST(test_sha_equal_detects_single_bit);
}
