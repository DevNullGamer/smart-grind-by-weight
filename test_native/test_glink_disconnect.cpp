#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "bluetooth/glink_disconnect.h"

// Host tests of the disconnect-reason text (src/bluetooth/glink_disconnect.*, chg023 part 1).
// NimBLE reports an HCI disconnect reason as 0x200 + the HCI code.

static constexpr size_t LONGEST_CAP = 48;

static void expect_describe(int nimble, const char* expected) {
    char buf[LONGEST_CAP];
    memset(buf, 'Z', sizeof(buf));
    glink_disconnect_describe(buf, sizeof(buf), nimble);
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

static void test_named_codes_via_describe() {
    expect_describe(0x200 + 0x08, "reason 0x08 (supervision timeout)");
    expect_describe(0x200 + 0x13, "reason 0x13 (remote user terminated)");
    expect_describe(0x200 + 0x14, "reason 0x14 (remote low resources)");
    expect_describe(0x200 + 0x15, "reason 0x15 (remote power off)");
    expect_describe(0x200 + 0x16, "reason 0x16 (local host terminated)");
    expect_describe(0x200 + 0x22, "reason 0x22 (LMP response timeout)");
    expect_describe(0x200 + 0x28, "reason 0x28 (instant passed)");
    expect_describe(0x200 + 0x3D, "reason 0x3D (MIC failure)");
    expect_describe(0x200 + 0x3E, "reason 0x3E (connection failed to establish)");
}

static void test_reason_name_lookup() {
    TEST_ASSERT_EQUAL_STRING("supervision timeout", glink_disconnect_reason_name(0x08));
    TEST_ASSERT_EQUAL_STRING("remote user terminated", glink_disconnect_reason_name(0x13));
    TEST_ASSERT_EQUAL_STRING("remote low resources", glink_disconnect_reason_name(0x14));
    TEST_ASSERT_EQUAL_STRING("remote power off", glink_disconnect_reason_name(0x15));
    TEST_ASSERT_EQUAL_STRING("local host terminated", glink_disconnect_reason_name(0x16));
    TEST_ASSERT_EQUAL_STRING("LMP response timeout", glink_disconnect_reason_name(0x22));
    TEST_ASSERT_EQUAL_STRING("instant passed", glink_disconnect_reason_name(0x28));
    TEST_ASSERT_EQUAL_STRING("MIC failure", glink_disconnect_reason_name(0x3D));
    TEST_ASSERT_EQUAL_STRING("connection failed to establish", glink_disconnect_reason_name(0x3E));
    TEST_ASSERT_NULL(glink_disconnect_reason_name(0x2A));
    TEST_ASSERT_NULL(glink_disconnect_reason_name(0xFF));
}

static void test_unnamed_hci_code() {
    expect_describe(0x22A, "reason 0x2A");
    expect_describe(0x2FF, "reason 0xFF");
    expect_describe(0, "reason none");
}

static void test_host_errors_are_not_hci() {
    char buf[LONGEST_CAP];
    glink_disconnect_describe(buf, sizeof(buf), 0x300);
    TEST_ASSERT_NOT_NULL(strstr(buf, "not an HCI code"));
    glink_disconnect_describe(buf, sizeof(buf), 0x1FF);
    TEST_ASSERT_NOT_NULL(strstr(buf, "not an HCI code"));
    glink_disconnect_describe(buf, sizeof(buf), 14);
    TEST_ASSERT_NOT_NULL(strstr(buf, "not an HCI code"));
}

static void test_negative_value_does_not_crash() {
    char buf[LONGEST_CAP];
    glink_disconnect_describe(buf, sizeof(buf), -1);
    TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) > 0);
    glink_disconnect_describe(buf, sizeof(buf), INT32_MIN);
    TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

static void test_short_buffer_is_terminated() {
    char buf[8];
    memset(buf, 'Z', sizeof(buf));
    glink_disconnect_describe(buf, sizeof(buf), 0x208);
    TEST_ASSERT_EQUAL(7, strlen(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("reason ", buf, 7);
    TEST_ASSERT_EQUAL('\0', buf[7]);
}

static void test_cap_one_gives_empty_string() {
    char buf[2] = {'Z', 'Z'};
    glink_disconnect_describe(buf, 1, 0x208);
    TEST_ASSERT_EQUAL('\0', buf[0]);
    TEST_ASSERT_EQUAL('Z', buf[1]);
}

static void test_cap_zero_and_null_are_safe() {
    char buf[2] = {'Z', 'Z'};
    glink_disconnect_describe(buf, 0, 0x208);
    TEST_ASSERT_EQUAL('Z', buf[0]);
    glink_disconnect_describe(nullptr, 0, 0x208);
    glink_disconnect_describe(nullptr, 16, 0x208);
}

static void test_longest_output_fits_48_bytes() {
    size_t longest = 0;
    char buf[LONGEST_CAP];
    for (int code = -300; code < 0x500; code++) {
        glink_disconnect_describe(buf, sizeof(buf), code);
        const size_t len = strlen(buf);
        if (len > longest) longest = len;
    }
    glink_disconnect_describe(buf, sizeof(buf), INT32_MAX);
    if (strlen(buf) > longest) longest = strlen(buf);
    glink_disconnect_describe(buf, sizeof(buf), INT32_MIN);
    if (strlen(buf) > longest) longest = strlen(buf);
    TEST_ASSERT_TRUE_MESSAGE(longest < LONGEST_CAP, "output must fit 48 bytes with the NUL");
    // And not truncated: the full text of every named code is intact at cap 48.
    expect_describe(0x200 + 0x3E, "reason 0x3E (connection failed to establish)");
}

static void test_tx_power_level_table() {
    const int expected[16] = {-24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 20};
    for (int level = 0; level < 16; level++) {
        int dbm = 12345;
        TEST_ASSERT_TRUE(glink_tx_power_level_dbm(level, &dbm));
        TEST_ASSERT_EQUAL_INT(expected[level], dbm);
    }
}

static void test_tx_power_level_invalid_leaves_dbm_untouched() {
    const int bad[] = {-1, 16, 0xFF, INT32_MIN};
    for (int level : bad) {
        int dbm = 12345;
        TEST_ASSERT_FALSE(glink_tx_power_level_dbm(level, &dbm));
        TEST_ASSERT_EQUAL_INT(12345, dbm);
    }
    TEST_ASSERT_FALSE(glink_tx_power_level_dbm(0, nullptr));
}

static void test_format_dbm_values() {
    char buf[16];
    glink_format_dbm(buf, sizeof(buf), 9);
    TEST_ASSERT_EQUAL_STRING("+9 dBm", buf);
    glink_format_dbm(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("+0 dBm", buf);
    glink_format_dbm(buf, sizeof(buf), -63);
    TEST_ASSERT_EQUAL_STRING("-63 dBm", buf);
    glink_format_dbm(buf, sizeof(buf), -128);
    TEST_ASSERT_EQUAL_STRING("-128 dBm", buf);
}

static void test_format_dbm_small_buffers() {
    char buf[16];
    memset(buf, 'Z', sizeof(buf));
    glink_format_dbm(buf, 1, 9);
    TEST_ASSERT_EQUAL('\0', buf[0]);
    TEST_ASSERT_EQUAL('Z', buf[1]);

    memset(buf, 'Z', sizeof(buf));
    glink_format_dbm(buf, 0, 9);
    TEST_ASSERT_EQUAL('Z', buf[0]);
    glink_format_dbm(nullptr, 0, 9);
    glink_format_dbm(nullptr, 16, 9);

    // "-128 dBm" is 8 characters: it needs cap 9; cap 8 truncates by one and stays terminated.
    memset(buf, 'Z', sizeof(buf));
    glink_format_dbm(buf, 9, -128);
    TEST_ASSERT_EQUAL_STRING("-128 dBm", buf);
    memset(buf, 'Z', sizeof(buf));
    glink_format_dbm(buf, 8, -128);
    TEST_ASSERT_EQUAL_STRING("-128 dB", buf);
    TEST_ASSERT_EQUAL('Z', buf[8]);

    memset(buf, 'Z', sizeof(buf));
    glink_format_dbm(buf, 5, -128);
    TEST_ASSERT_EQUAL_STRING("-128", buf);
    TEST_ASSERT_EQUAL('Z', buf[5]);
}

void run_glink_disconnect_tests() {
    RUN_TEST(test_tx_power_level_table);
    RUN_TEST(test_tx_power_level_invalid_leaves_dbm_untouched);
    RUN_TEST(test_format_dbm_values);
    RUN_TEST(test_format_dbm_small_buffers);
    RUN_TEST(test_named_codes_via_describe);
    RUN_TEST(test_reason_name_lookup);
    RUN_TEST(test_unnamed_hci_code);
    RUN_TEST(test_host_errors_are_not_hci);
    RUN_TEST(test_negative_value_does_not_crash);
    RUN_TEST(test_short_buffer_is_terminated);
    RUN_TEST(test_cap_one_gives_empty_string);
    RUN_TEST(test_cap_zero_and_null_are_safe);
    RUN_TEST(test_longest_output_fits_48_bytes);
}
