#include <unity.h>
#include <string.h>
#include "network/update_prompt_text.h"

static void test_sanitize_printable_and_replacement() {
    char out[40];
    TEST_ASSERT_EQUAL_STRING("1.5.0-rc.1", prompt_sanitize("1.5.0-rc.1", 32, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a?b?c", prompt_sanitize("a\nb\tc", 32, out, sizeof(out)));
    // Every byte of a UTF-8 sequence is replaced
    TEST_ASSERT_EQUAL_STRING("caf??", prompt_sanitize("caf\xC3\xA9", 32, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("?", prompt_sanitize("\x7F", 32, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("~ !", prompt_sanitize("~ !", 32, out, sizeof(out)));
}

static void test_sanitize_limits_and_null() {
    char out[40];
    TEST_ASSERT_EQUAL_STRING("abcde", prompt_sanitize("abcdefghij", 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", prompt_sanitize(nullptr, 5, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", prompt_sanitize("", 5, out, sizeof(out)));
    char small[4];
    TEST_ASSERT_EQUAL_STRING("abc", prompt_sanitize("abcdefghij", 32, small, sizeof(small)));
    TEST_ASSERT_NULL(prompt_sanitize("x", 5, nullptr, 0));
    char one[1] = {'x'};
    prompt_sanitize("abc", 5, one, 0);
    TEST_ASSERT_EQUAL_CHAR('x', one[0]);  // zero length: untouched
    prompt_sanitize("abc", 5, one, 1);
    TEST_ASSERT_EQUAL_CHAR('\0', one[0]);
}

static void test_format_size() {
    char out[16];
    TEST_ASSERT_EQUAL_STRING("0 KB", prompt_format_size(0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0 KB", prompt_format_size(1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1 KB", prompt_format_size(500, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("812 KB", prompt_format_size(812000, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("999 KB", prompt_format_size(999499, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1 MB", prompt_format_size(999500, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1 MB", prompt_format_size(1000000, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2 MB", prompt_format_size(2000000, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2.2 MB", prompt_format_size(2191088, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2.1 MB", prompt_format_size(2149999, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2.2 MB", prompt_format_size(2150000, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3 MB", prompt_format_size(2960000, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("4295 MB", prompt_format_size(0xFFFFFFFFu, out, sizeof(out)));
    char tiny[4];
    prompt_format_size(2191088, tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[3]);
}

static void test_message_full() {
    char out[UPDATE_PROMPT_MESSAGE_BUF];
    prompt_build_message(2191088, 245, "1.5.0", "192.168.1.20", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "Install new firmware?\nv1.5.0 (build 245)\n2.2 MB from 192.168.1.20\nThe grinder restarts when done.", out);
}

static void test_message_missing_parts() {
    char out[UPDATE_PROMPT_MESSAGE_BUF];
    prompt_build_message(2191088, 245, "", "192.168.1.20", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Install new firmware?\nbuild 245\n2.2 MB from 192.168.1.20\nThe grinder restarts when done.", out);
    prompt_build_message(2191088, -1, "1.5.0", "192.168.1.20", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Install new firmware?\nv1.5.0\n2.2 MB from 192.168.1.20\nThe grinder restarts when done.", out);
    prompt_build_message(2191088, -1, nullptr, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Install new firmware?\n2.2 MB\nThe grinder restarts when done.", out);
    prompt_build_message(0, 0, "dev", nullptr, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Install new firmware?\ndev (build 0)\n0 KB\nThe grinder restarts when done.", out);
}

static void test_message_hostile_strings() {
    char out[UPDATE_PROMPT_MESSAGE_BUF];
    const char* version = "9\n\n\n<script>\xFF\xFE%s%n%d----------------------------------------";
    prompt_build_message(2191088, 2147483647, version, "999.999.999.999\nX%s", out, sizeof(out));
    // Exactly the three line breaks of the template, none from the client
    int newlines = 0;
    for (const char* p = out; *p; p++) newlines += (*p == '\n');
    TEST_ASSERT_EQUAL_INT(3, newlines);
    TEST_ASSERT_TRUE(strlen(out) < UPDATE_PROMPT_MESSAGE_BUF);
    TEST_ASSERT_NOT_NULL(strstr(out, "(build 2147483647)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "The grinder restarts when done."));
    for (const char* p = out; *p; p++) {
        const unsigned char c = static_cast<unsigned char>(*p);
        TEST_ASSERT_TRUE(c == '\n' || (c >= 0x20 && c <= 0x7E));
    }
}

static void test_message_longest_fits_the_constant_buffer() {
    char out[UPDATE_PROMPT_MESSAGE_BUF];
    char version[64];
    memset(version, 'W', sizeof(version) - 1);
    version[sizeof(version) - 1] = '\0';
    prompt_build_message(0xFFFFFFFFu, 2147483647, version, "255.255.255.255", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "The grinder restarts when done."));  // not truncated
    TEST_ASSERT_TRUE(strlen(out) < sizeof(out));
}

static void test_message_small_buffer_is_terminated_and_bounded() {
    char canary[16];
    for (size_t cap = 1; cap < 12; cap++) {
        memset(canary, 'Z', sizeof(canary));
        prompt_build_message(2191088, 245, "1.5.0", "192.168.1.20", canary, cap);
        TEST_ASSERT_TRUE(strlen(canary) < cap);
        for (size_t i = cap; i < sizeof(canary); i++) TEST_ASSERT_EQUAL_CHAR('Z', canary[i]);  // nothing written past cap
    }
    TEST_ASSERT_NULL(prompt_build_message(1, 1, "1", "1", nullptr, 0));
}

static void test_version_prefix_only_for_digits() {
    char out[UPDATE_PROMPT_MESSAGE_BUF];
    prompt_build_message(2000000, -1, "v1.5.0", "", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "\nv1.5.0\n"));
    TEST_ASSERT_NULL(strstr(out, "vv1.5.0"));
}

void run_update_prompt_text_tests() {
    RUN_TEST(test_sanitize_printable_and_replacement);
    RUN_TEST(test_sanitize_limits_and_null);
    RUN_TEST(test_format_size);
    RUN_TEST(test_message_full);
    RUN_TEST(test_message_missing_parts);
    RUN_TEST(test_message_hostile_strings);
    RUN_TEST(test_message_longest_fits_the_constant_buffer);
    RUN_TEST(test_message_small_buffer_is_terminated_and_bounded);
    RUN_TEST(test_version_prefix_only_for_digits);
}
