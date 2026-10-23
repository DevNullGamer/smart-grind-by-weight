#include <unity.h>
#include <string.h>
#include "network/wifi_status_text.h"

static void test_status_text_every_state() {
    char buf[WIFI_STATUS_TEXT_BUF];
    TEST_ASSERT_EQUAL_STRING("Not set up", wifi_status_text(WiFiState::NO_CREDENTIALS, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Connecting...", wifi_status_text(WiFiState::CONNECTING, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Connected", wifi_status_text(WiFiState::CONNECTED, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Retry in 20s", wifi_status_text(WiFiState::WAITING_RETRY, 20000, buf, sizeof(buf)));
}

static void test_status_text_retry_rounds_up_and_never_zero() {
    char buf[WIFI_STATUS_TEXT_BUF];
    TEST_ASSERT_EQUAL_STRING("Retry in 1s", wifi_status_text(WiFiState::WAITING_RETRY, 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Retry in 1s", wifi_status_text(WiFiState::WAITING_RETRY, 1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Retry in 1s", wifi_status_text(WiFiState::WAITING_RETRY, 1000, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Retry in 2s", wifi_status_text(WiFiState::WAITING_RETRY, 1001, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Retry in 60s", wifi_status_text(WiFiState::WAITING_RETRY, 60000, buf, sizeof(buf)));
    // Retry time is ignored outside WAITING_RETRY
    TEST_ASSERT_EQUAL_STRING("Connected", wifi_status_text(WiFiState::CONNECTED, 5000, buf, sizeof(buf)));
}

static void test_status_text_small_buffer_is_terminated() {
    char buf[6];
    memset(buf, 'x', sizeof(buf));
    wifi_status_text(WiFiState::CONNECTING, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
    TEST_ASSERT_EQUAL_STRING("Conne", buf);
    // Null / empty destination must not crash
    wifi_status_text(WiFiState::CONNECTED, 0, nullptr, 0);
    wifi_status_text(WiFiState::WAITING_RETRY, 3000, buf, 0);
}

static void test_rssi_quality_boundaries() {
    TEST_ASSERT_EQUAL_STRING("Excellent", wifi_rssi_quality(0));
    TEST_ASSERT_EQUAL_STRING("Excellent", wifi_rssi_quality(-55));
    TEST_ASSERT_EQUAL_STRING("Good", wifi_rssi_quality(-56));
    TEST_ASSERT_EQUAL_STRING("Good", wifi_rssi_quality(-67));
    TEST_ASSERT_EQUAL_STRING("Fair", wifi_rssi_quality(-68));
    TEST_ASSERT_EQUAL_STRING("Fair", wifi_rssi_quality(-75));
    TEST_ASSERT_EQUAL_STRING("Weak", wifi_rssi_quality(-76));
    TEST_ASSERT_EQUAL_STRING("Weak", wifi_rssi_quality(-90));
}

static void test_signal_text() {
    char buf[WIFI_STATUS_TEXT_BUF];
    TEST_ASSERT_EQUAL_STRING("-62 dBm Good", wifi_signal_text(-62, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("-55 dBm Excellent", wifi_signal_text(-55, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("-90 dBm Weak", wifi_signal_text(-90, buf, sizeof(buf)));
    // 0 = not connected, positive = invalid
    TEST_ASSERT_EQUAL_STRING("--", wifi_signal_text(0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("--", wifi_signal_text(5, buf, sizeof(buf)));
    // The longest realistic text fits the documented buffer
    TEST_ASSERT_TRUE(strlen(wifi_signal_text(-100, buf, sizeof(buf))) < WIFI_STATUS_TEXT_BUF);
}

static void test_shorten_ssid_short_names_unchanged() {
    char buf[WIFI_STATUS_TEXT_BUF];
    TEST_ASSERT_EQUAL_STRING("HomeNet", wifi_shorten_ssid("HomeNet", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("with space!", wifi_shorten_ssid("with space!", buf, sizeof(buf)));
    // Exactly the display maximum (20) is not shortened
    TEST_ASSERT_EQUAL_STRING("12345678901234567890", wifi_shorten_ssid("12345678901234567890", buf, sizeof(buf)));
}

static void test_shorten_ssid_long_names_get_ellipsis() {
    char buf[WIFI_STATUS_TEXT_BUF];
    TEST_ASSERT_EQUAL_STRING("12345678901234567...", wifi_shorten_ssid("123456789012345678901", buf, sizeof(buf)));
}

static void test_shorten_ssid_32_byte_name() {
    char buf[WIFI_STATUS_TEXT_BUF];
    const char* full = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";  // 32 bytes (SSID maximum)
    TEST_ASSERT_EQUAL_UINT(32, strlen(full));
    wifi_shorten_ssid(full, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOPQ...", buf);
    TEST_ASSERT_EQUAL_UINT(WIFI_STATUS_SSID_DISPLAY_MAX, strlen(buf));
}

static void test_shorten_ssid_empty_and_null() {
    char buf[WIFI_STATUS_TEXT_BUF];
    TEST_ASSERT_EQUAL_STRING("--", wifi_shorten_ssid("", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("--", wifi_shorten_ssid(nullptr, buf, sizeof(buf)));
}

static void test_shorten_ssid_replaces_control_and_utf8_bytes() {
    char buf[WIFI_STATUS_TEXT_BUF];
    // Control characters and DEL
    TEST_ASSERT_EQUAL_STRING("a?b?c", wifi_shorten_ssid("a\tb\x7f" "c", buf, sizeof(buf)));
    // A multi-byte UTF-8 character (two bytes) becomes a single '?'
    TEST_ASSERT_EQUAL_STRING("Caf?", wifi_shorten_ssid("Caf\xc3\xa9", buf, sizeof(buf)));
    // Run of three-byte characters collapses to one '?'
    TEST_ASSERT_EQUAL_STRING("?", wifi_shorten_ssid("\xe2\x82\xac\xe2\x82\xac", buf, sizeof(buf)));
    // Sanitising happens before shortening: 32 non-ASCII bytes alternate with ASCII
    char mixed[33];
    for (int i = 0; i < 32; i += 2) {
        mixed[i] = 'a';
        mixed[i + 1] = static_cast<char>(0xC3);
    }
    mixed[32] = '\0';
    wifi_shorten_ssid(mixed, buf, sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) <= WIFI_STATUS_SSID_DISPLAY_MAX);
    for (const char* p = buf; *p; p++) TEST_ASSERT_TRUE(*p >= 0x20 && *p <= 0x7E);
}

static void test_shorten_ssid_small_buffer_is_terminated() {
    char buf[8];
    memset(buf, 'x', sizeof(buf));
    wifi_shorten_ssid("LongNetworkName", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
    TEST_ASSERT_EQUAL_STRING("LongNet", buf);
    wifi_shorten_ssid("x", nullptr, 0);
}

void run_wifi_status_text_tests() {
    RUN_TEST(test_status_text_every_state);
    RUN_TEST(test_status_text_retry_rounds_up_and_never_zero);
    RUN_TEST(test_status_text_small_buffer_is_terminated);
    RUN_TEST(test_rssi_quality_boundaries);
    RUN_TEST(test_signal_text);
    RUN_TEST(test_shorten_ssid_short_names_unchanged);
    RUN_TEST(test_shorten_ssid_long_names_get_ellipsis);
    RUN_TEST(test_shorten_ssid_32_byte_name);
    RUN_TEST(test_shorten_ssid_empty_and_null);
    RUN_TEST(test_shorten_ssid_replaces_control_and_utf8_bytes);
    RUN_TEST(test_shorten_ssid_small_buffer_is_terminated);
}
