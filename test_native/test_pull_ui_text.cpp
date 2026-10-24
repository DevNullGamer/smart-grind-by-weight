#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "network/pull_ui_text.h"
#include "network/update_ui_logic.h"

static const PullState kAllStates[] = {PullState::IDLE,        PullState::CHECKING,    PullState::UP_TO_DATE,
                                       PullState::AVAILABLE,   PullState::CHECK_FAILED, PullState::DOWNLOADING,
                                       PullState::VERIFYING,   PullState::INSTALLED,   PullState::FAILED};

static const uint32_t kStale = UPDATE_PULL_RESULT_STALE_MS;
static const char* const kV32 = "1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaaa";   // 32 chars, the longest accepted version

// Fresh result, WiFi up, Bluetooth on, offer v1.5.0
static PullPageInput base_input(PullState state) {
    PullPageInput in;
    in.state = state;
    in.wifi_connected = true;
    in.ble_enabled = true;
    in.offer_version = "1.5.0";
    in.result_ms = 1000;
    in.now_ms = 2000;
    return in;
}

static PullPageView view_of(const PullPageInput& in) {
    PullPageView v;
    memset(&v, 0x55, sizeof(v));   // detect fields that are not written
    pull_page_view(in, &v);
    return v;
}

//------------------------------------------------------------------------------
// pull_installed_text
//------------------------------------------------------------------------------
static void test_installed_text_formats() {
    char buf[PULL_UI_VERSION_BUF + 16];
    TEST_ASSERT_EQUAL_STRING("v1.4.1-rc.3 (#217)", pull_installed_text("1.4.1-rc.3", 217, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("v1.4.1", pull_installed_text("1.4.1", -1, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("v1.4.1 (#0)", pull_installed_text("1.4.1", 0, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("dev (#5)", pull_installed_text("dev", 5, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("--", pull_installed_text(nullptr, 5, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("--", pull_installed_text("", 5, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(buf == pull_installed_text("1.0.0", 1, buf, sizeof(buf)));
}

static void test_installed_text_truncates_safely() {
    TEST_ASSERT_EQUAL_UINT(32, strlen(kV32));
    for (size_t len = 1; len < 12; len++) {
        char guard[64];
        memset(guard, 'X', sizeof(guard));
        pull_installed_text(kV32, 123456, guard, len);
        TEST_ASSERT_TRUE(strnlen(guard, len) < len);   // NUL inside the buffer
        TEST_ASSERT_EQUAL_CHAR('X', guard[len]);       // nothing written beyond
    }
    char big[PULL_UI_VERSION_BUF + 16];
    pull_installed_text(kV32, 123456, big, sizeof(big));
    TEST_ASSERT_EQUAL_STRING("v1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaaa (#123456)", big);
    char one[1] = {'X'};
    pull_installed_text("1.0.0", 1, one, 0);   // size 0: untouched, no crash
    TEST_ASSERT_EQUAL_CHAR('X', one[0]);
    TEST_ASSERT_NULL(pull_installed_text("1.0.0", 1, nullptr, 10));
}

//------------------------------------------------------------------------------
// pull_result_is_stale
//------------------------------------------------------------------------------
static void test_stale_boundary() {
    TEST_ASSERT_FALSE(pull_result_is_stale(1000, 1000, true));
    TEST_ASSERT_FALSE(pull_result_is_stale(1000, 1000 + kStale - 1, true));
    TEST_ASSERT_FALSE(pull_result_is_stale(1000, 1000 + kStale, true));
    TEST_ASSERT_TRUE(pull_result_is_stale(1000, 1000 + kStale + 1, true));
}

static void test_stale_wifi_lost() {
    TEST_ASSERT_TRUE(pull_result_is_stale(1000, 1000, false));
    TEST_ASSERT_TRUE(pull_result_is_stale(1000, 5000, false));
}

static void test_stale_across_millis_wrap() {
    const uint32_t result = 0xFFFFFF00u;
    TEST_ASSERT_FALSE(pull_result_is_stale(result, 0x00000100u, true));        // 512 ms later
    TEST_ASSERT_FALSE(pull_result_is_stale(result, result + kStale, true));    // exactly at the limit, wrapped
    TEST_ASSERT_TRUE(pull_result_is_stale(result, result + kStale + 1, true)); // just over, wrapped
    TEST_ASSERT_TRUE(pull_result_is_stale(0xFFFFFFFFu, 0xFFFFFFFFu + kStale + 5, true));
}

//------------------------------------------------------------------------------
// pull_page_view
//------------------------------------------------------------------------------
static void test_view_idle() {
    PullPageView v = view_of(base_input(PullState::IDLE));
    TEST_ASSERT_EQUAL_STRING("--", v.latest);
    TEST_ASSERT_EQUAL(PullTone::NORMAL, v.latest_tone);
    TEST_ASSERT_EQUAL_STRING("Check for update", v.check_label);
    TEST_ASSERT_TRUE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("", v.install_label);
    TEST_ASSERT_EQUAL_STRING("", v.notice);
}

static void test_view_checking() {
    PullPageView v = view_of(base_input(PullState::CHECKING));
    TEST_ASSERT_EQUAL_STRING("Checking...", v.latest);
    TEST_ASSERT_EQUAL(PullTone::NORMAL, v.latest_tone);
    TEST_ASSERT_EQUAL_STRING("Checking...", v.check_label);
    TEST_ASSERT_FALSE(v.check_enabled);
    TEST_ASSERT_FALSE(v.beta_enabled);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("", v.notice);
}

static void test_view_up_to_date_and_no_release() {
    PullPageInput in = base_input(PullState::UP_TO_DATE);
    PullPageView v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("Up to date", v.latest);
    TEST_ASSERT_EQUAL(PullTone::NORMAL, v.latest_tone);
    TEST_ASSERT_TRUE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_FALSE(v.show_install);
    in.no_release = true;
    v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("No release published", v.latest);
}

static void test_view_available_fresh() {
    PullPageView v = view_of(base_input(PullState::AVAILABLE));
    TEST_ASSERT_EQUAL_STRING("v1.5.0", v.latest);
    TEST_ASSERT_EQUAL(PullTone::GOOD, v.latest_tone);
    TEST_ASSERT_EQUAL_STRING("Check for update", v.check_label);
    TEST_ASSERT_TRUE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_TRUE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("Install v1.5.0", v.install_label);
    TEST_ASSERT_EQUAL_STRING("", v.notice);
}

static void test_view_available_version_with_v_prefix_not_doubled() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.offer_version = "v1.5.0";
    PullPageView v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("v1.5.0", v.latest);
    TEST_ASSERT_EQUAL_STRING("Install v1.5.0", v.install_label);
}

static void test_view_available_long_prerelease_fits() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.offer_version = kV32;
    PullPageView v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("v1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaaa", v.latest);
    TEST_ASSERT_EQUAL_STRING("Install v1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaaa", v.install_label);
}

static void test_view_available_stale_by_age() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.now_ms = in.result_ms + kStale;   // boundary: still fresh
    TEST_ASSERT_TRUE(view_of(in).show_install);
    in.now_ms = in.result_ms + kStale + 1;
    PullPageView v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("Check again", v.latest);
    TEST_ASSERT_EQUAL(PullTone::NORMAL, v.latest_tone);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("", v.install_label);
    TEST_ASSERT_TRUE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_EQUAL_STRING("This result is old. Check again.", v.notice);
}

static void test_view_available_stale_across_wrap() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.result_ms = 0xFFFFFF00u;
    in.now_ms = 0x100u;
    TEST_ASSERT_TRUE(view_of(in).show_install);
    in.now_ms = in.result_ms + kStale + 1;
    TEST_ASSERT_FALSE(view_of(in).show_install);
}

static void test_view_available_wifi_lost() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.wifi_connected = false;
    PullPageView v = view_of(in);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("Check again", v.latest);
    TEST_ASSERT_FALSE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_EQUAL_STRING("Connect to WiFi to check for updates.", v.notice);
}

static void test_view_available_without_offer() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.offer_version = nullptr;
    PullPageView v = view_of(in);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("", v.install_label);
    TEST_ASSERT_EQUAL_STRING("Check again", v.latest);
    in.offer_version = "";
    v = view_of(in);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("Check again", v.latest);
}

static void test_view_check_failed() {
    PullPageInput in = base_input(PullState::CHECK_FAILED);
    in.failure = PullFailure::NO_CONNECTION;
    PullPageView v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("Check failed", v.latest);
    TEST_ASSERT_EQUAL(PullTone::WARN, v.latest_tone);
    TEST_ASSERT_TRUE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_FALSE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("Could not reach the update server.", v.notice);

    in.failure = PullFailure::NOT_FOUND;
    v = view_of(in);
    char expected[PULL_UI_NOTICE_BUF];
    snprintf(expected, sizeof(expected), "Check failed: %s.", pull_failure_text(PullFailure::NOT_FOUND));
    TEST_ASSERT_EQUAL_STRING(expected, v.notice);
}

static void test_view_running_states() {
    const PullState running[] = {PullState::DOWNLOADING, PullState::VERIFYING, PullState::INSTALLED};
    for (PullState s : running) {
        PullPageView v = view_of(base_input(s));
        TEST_ASSERT_EQUAL_STRING("Installing...", v.latest);
        TEST_ASSERT_EQUAL(PullTone::NORMAL, v.latest_tone);
        TEST_ASSERT_FALSE(v.check_enabled);
        TEST_ASSERT_FALSE(v.beta_enabled);
        TEST_ASSERT_FALSE(v.show_install);
        TEST_ASSERT_EQUAL_STRING("Check for update", v.check_label);
    }
}

static void test_view_failed() {
    PullPageInput in = base_input(PullState::FAILED);
    in.failure = PullFailure::CHECKSUM_MISMATCH;
    PullPageView v = view_of(in);
    TEST_ASSERT_EQUAL_STRING("Install failed", v.latest);
    TEST_ASSERT_EQUAL(PullTone::WARN, v.latest_tone);
    TEST_ASSERT_TRUE(v.check_enabled);
    TEST_ASSERT_TRUE(v.beta_enabled);
    TEST_ASSERT_FALSE(v.show_install);
    char expected[PULL_UI_NOTICE_BUF];
    snprintf(expected, sizeof(expected), "Install failed: %s.", pull_failure_text(PullFailure::CHECKSUM_MISMATCH));
    TEST_ASSERT_EQUAL_STRING(expected, v.notice);
}

static void test_view_failed_bluetooth_off_hint() {
    PullPageInput in = base_input(PullState::FAILED);
    in.failure = PullFailure::CONNECTION_LOST;
    in.ble_enabled = false;
    PullPageView v = view_of(in);
    TEST_ASSERT_NOT_NULL(strstr(v.notice, "Install failed: "));
    TEST_ASSERT_NOT_NULL(strstr(v.notice, "Bluetooth is off"));
    TEST_ASSERT_NOT_NULL(strstr(v.notice, "Bluetooth page"));
    TEST_ASSERT_TRUE(strlen(v.notice) < PULL_UI_NOTICE_BUF);
    // Bluetooth on: no hint
    in.ble_enabled = true;
    TEST_ASSERT_NULL(strstr(view_of(in).notice, "Bluetooth"));
}

static void test_view_bluetooth_off_only_matters_after_failed_install() {
    const PullState states[] = {PullState::IDLE, PullState::UP_TO_DATE, PullState::AVAILABLE, PullState::CHECK_FAILED};
    for (PullState s : states) {
        PullPageInput in = base_input(s);
        in.ble_enabled = false;
        in.failure = PullFailure::NO_CONNECTION;
        TEST_ASSERT_NULL(strstr(view_of(in).notice, "Bluetooth"));
    }
}

static void test_view_no_wifi_every_state() {
    for (PullState s : kAllStates) {
        PullPageInput in = base_input(s);
        in.wifi_connected = false;
        in.failure = PullFailure::NO_CONNECTION;
        PullPageView v = view_of(in);
        TEST_ASSERT_FALSE(v.check_enabled);
        TEST_ASSERT_FALSE(v.show_install);
        TEST_ASSERT_EQUAL(pull_can_start_check(s), v.beta_enabled);
        if (s == PullState::FAILED || s == PullState::CHECK_FAILED) {
            TEST_ASSERT_NULL(strstr(v.notice, "Connect to WiFi"));   // the failure explanation wins
        } else if (pull_state_is_running(s)) {
            TEST_ASSERT_EQUAL_STRING("", v.notice);
        } else {
            TEST_ASSERT_EQUAL_STRING("Connect to WiFi to check for updates.", v.notice);
        }
    }
}

static void test_view_check_and_beta_follow_can_start_check() {
    for (PullState s : kAllStates) {
        PullPageView v = view_of(base_input(s));
        TEST_ASSERT_EQUAL(pull_can_start_check(s), v.check_enabled);
        TEST_ASSERT_EQUAL(pull_can_start_check(s), v.beta_enabled);
        TEST_ASSERT_EQUAL_STRING(s == PullState::CHECKING ? "Checking..." : "Check for update", v.check_label);
        if (s != PullState::AVAILABLE) TEST_ASSERT_FALSE(v.show_install);
    }
}

static void test_view_notice_has_priority() {
    for (PullState s : kAllStates) {
        PullPageInput in = base_input(s);
        in.failure = PullFailure::CHECKSUM_MISMATCH;
        in.notice = PullFailure::BLE_CONNECTED;
        in.wifi_connected = (s != PullState::IDLE);
        PullPageView v = view_of(in);
        char expected[PULL_UI_NOTICE_BUF];
        snprintf(expected, sizeof(expected), "%s.", pull_failure_text(PullFailure::BLE_CONNECTED));
        TEST_ASSERT_EQUAL_STRING(expected, v.notice);
    }
}

static void test_view_notice_does_not_change_buttons() {
    PullPageInput in = base_input(PullState::AVAILABLE);
    in.notice = PullFailure::BLE_CONNECTED;
    PullPageView v = view_of(in);
    TEST_ASSERT_TRUE(v.show_install);
    TEST_ASSERT_EQUAL_STRING("v1.5.0", v.latest);
}

static void test_view_all_fields_terminated_for_all_failures() {
    for (PullState s : kAllStates) {
        for (int f = 0; f <= static_cast<int>(PullFailure::INTERNAL); f++) {
            PullPageInput in = base_input(s);
            in.failure = static_cast<PullFailure>(f);
            in.notice = static_cast<PullFailure>(f);
            in.ble_enabled = false;
            in.offer_version = kV32;
            PullPageView v = view_of(in);
            TEST_ASSERT_TRUE(strnlen(v.latest, sizeof(v.latest)) < sizeof(v.latest));
            TEST_ASSERT_TRUE(strnlen(v.check_label, sizeof(v.check_label)) < sizeof(v.check_label));
            TEST_ASSERT_TRUE(strnlen(v.install_label, sizeof(v.install_label)) < sizeof(v.install_label));
            TEST_ASSERT_TRUE(strnlen(v.notice, sizeof(v.notice)) < sizeof(v.notice));
        }
    }
}

static void test_view_null_out_is_safe() {
    pull_page_view(base_input(PullState::IDLE), nullptr);
    TEST_PASS();
}

//------------------------------------------------------------------------------
// pull_confirm_message
//------------------------------------------------------------------------------
static void test_confirm_message_layout() {
    char buf[PULL_UI_CONFIRM_BUF];
    pull_confirm_message("1.5.0", "1.4.1-rc.3", 2200000, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(
        "Install v1.5.0?\nNow: v1.4.1-rc.3\nSize: 2.2 MB\n\nBluetooth is turned off meanwhile. The grinder restarts when done.", buf);
    pull_confirm_message("v1.5.0", "v1.4.1", 500000, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Install v1.5.0?\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Now: v1.4.1\n"));
    TEST_ASSERT_NULL(strstr(buf, "vv"));
    pull_confirm_message(nullptr, nullptr, 0, buf, sizeof(buf));
    TEST_ASSERT_TRUE(strlen(buf) < sizeof(buf));
}

static void test_confirm_message_max_versions_fit_default_buffer() {
    char buf[PULL_UI_CONFIRM_BUF];
    pull_confirm_message(kV32, kV32, 4000000000u, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "restarts when done."));   // not cut
    TEST_ASSERT_NOT_NULL(strstr(buf, "Install v1.2.3-aaaaaaaaaaaaaaaaaaaaaaaaaa?"));
}

static void test_confirm_message_small_buffers() {
    for (size_t len = 1; len < 80; len++) {
        char guard[300];
        memset(guard, 'X', sizeof(guard));
        pull_confirm_message(kV32, kV32, 2200000, guard, len);
        TEST_ASSERT_TRUE(strnlen(guard, len) < len);   // NUL terminated inside the buffer
        TEST_ASSERT_EQUAL_CHAR('X', guard[len]);       // no overflow
    }
    char one[1] = {'X'};
    pull_confirm_message("1.5.0", "1.4.1", 1, one, 0);
    TEST_ASSERT_EQUAL_CHAR('X', one[0]);
    TEST_ASSERT_NULL(pull_confirm_message("1.5.0", "1.4.1", 1, nullptr, 10));
}

//------------------------------------------------------------------------------
// update_ui_logic: pull progress
//------------------------------------------------------------------------------
static void test_pull_progress_view_connecting() {
    // Pull job running, UpdateStatus has nothing yet
    TEST_ASSERT_EQUAL(PullProgressView::CONNECTING, update_pull_progress_view(PullState::DOWNLOADING, UpdatePhase::IDLE, false));
    TEST_ASSERT_EQUAL(PullProgressView::CONNECTING,
                      update_pull_progress_view(PullState::DOWNLOADING, UpdatePhase::AWAITING_APPROVAL, false));
    TEST_ASSERT_EQUAL(PullProgressView::CONNECTING, update_pull_progress_view(PullState::VERIFYING, UpdatePhase::IDLE, false));
}

static void test_pull_progress_view_status_phases_win() {
    const UpdatePhase phases[] = {UpdatePhase::RECEIVING, UpdatePhase::VERIFYING, UpdatePhase::INSTALLED, UpdatePhase::FAILED};
    for (UpdatePhase p : phases) {
        for (PullState s : kAllStates) {
            TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(s, p, false));
            TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(s, p, true));
        }
    }
}

static void test_pull_progress_view_installed_state() {
    // Installed is reported by UpdateStatus; with nothing there the pull view adds nothing
    TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(PullState::INSTALLED, UpdatePhase::IDLE, false));
    TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(PullState::INSTALLED, UpdatePhase::INSTALLED, false));
}

static void test_pull_progress_view_failure_not_in_status() {
    TEST_ASSERT_EQUAL(PullProgressView::FAILED, update_pull_progress_view(PullState::FAILED, UpdatePhase::IDLE, false));
    TEST_ASSERT_EQUAL(PullProgressView::FAILED,
                      update_pull_progress_view(PullState::FAILED, UpdatePhase::AWAITING_APPROVAL, false));
    // Shown once, then acknowledged
    TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(PullState::FAILED, UpdatePhase::IDLE, true));
}

static void test_pull_progress_view_other_states_none() {
    const PullState others[] = {PullState::IDLE,      PullState::CHECKING,     PullState::UP_TO_DATE,
                                PullState::AVAILABLE, PullState::CHECK_FAILED, PullState::INSTALLED};
    for (PullState s : others) {
        TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(s, UpdatePhase::IDLE, false));
        TEST_ASSERT_EQUAL(PullProgressView::NONE, update_pull_progress_view(s, UpdatePhase::AWAITING_APPROVAL, false));
    }
}

static void test_progress_title_and_connecting_text() {
    TEST_ASSERT_EQUAL_STRING("Downloading", update_progress_title(true));
    TEST_ASSERT_EQUAL_STRING("WiFi update", update_progress_title(false));
    TEST_ASSERT_EQUAL_STRING("Connecting...", update_pull_connecting_text());
}

static void test_progress_status_text_pull_vs_push() {
    char buf[UPDATE_UI_STATUS_BUF];
    TEST_ASSERT_EQUAL_STRING("Downloading update...",
                             update_progress_status_text(UpdatePhase::RECEIVING, nullptr, buf, sizeof(buf), true));
    TEST_ASSERT_EQUAL_STRING("Receiving update...",
                             update_progress_status_text(UpdatePhase::RECEIVING, nullptr, buf, sizeof(buf), false));
    TEST_ASSERT_EQUAL_STRING("Receiving update...", update_progress_status_text(UpdatePhase::RECEIVING, nullptr, buf, sizeof(buf)));
    // Other phases read the same for both
    for (int pull = 0; pull < 2; pull++) {
        const bool p = pull != 0;
        TEST_ASSERT_EQUAL_STRING("Verifying...", update_progress_status_text(UpdatePhase::VERIFYING, nullptr, buf, sizeof(buf), p));
        TEST_ASSERT_EQUAL_STRING("Installed. Restarting...",
                                 update_progress_status_text(UpdatePhase::INSTALLED, nullptr, buf, sizeof(buf), p));
        TEST_ASSERT_EQUAL_STRING("Failed: Checksum mism...",
                                 update_progress_status_text(UpdatePhase::FAILED, "Checksum mismatch", buf, sizeof(buf), p));
        TEST_ASSERT_EQUAL_STRING("", update_progress_status_text(UpdatePhase::IDLE, "x", buf, sizeof(buf), p));
        TEST_ASSERT_EQUAL_STRING("", update_progress_status_text(UpdatePhase::AWAITING_APPROVAL, "x", buf, sizeof(buf), p));
    }
}

void run_pull_ui_text_tests() {
    RUN_TEST(test_installed_text_formats);
    RUN_TEST(test_installed_text_truncates_safely);
    RUN_TEST(test_stale_boundary);
    RUN_TEST(test_stale_wifi_lost);
    RUN_TEST(test_stale_across_millis_wrap);
    RUN_TEST(test_view_idle);
    RUN_TEST(test_view_checking);
    RUN_TEST(test_view_up_to_date_and_no_release);
    RUN_TEST(test_view_available_fresh);
    RUN_TEST(test_view_available_version_with_v_prefix_not_doubled);
    RUN_TEST(test_view_available_long_prerelease_fits);
    RUN_TEST(test_view_available_stale_by_age);
    RUN_TEST(test_view_available_stale_across_wrap);
    RUN_TEST(test_view_available_wifi_lost);
    RUN_TEST(test_view_available_without_offer);
    RUN_TEST(test_view_check_failed);
    RUN_TEST(test_view_running_states);
    RUN_TEST(test_view_failed);
    RUN_TEST(test_view_failed_bluetooth_off_hint);
    RUN_TEST(test_view_bluetooth_off_only_matters_after_failed_install);
    RUN_TEST(test_view_no_wifi_every_state);
    RUN_TEST(test_view_check_and_beta_follow_can_start_check);
    RUN_TEST(test_view_notice_has_priority);
    RUN_TEST(test_view_notice_does_not_change_buttons);
    RUN_TEST(test_view_all_fields_terminated_for_all_failures);
    RUN_TEST(test_view_null_out_is_safe);
    RUN_TEST(test_confirm_message_layout);
    RUN_TEST(test_confirm_message_max_versions_fit_default_buffer);
    RUN_TEST(test_confirm_message_small_buffers);
    RUN_TEST(test_pull_progress_view_connecting);
    RUN_TEST(test_pull_progress_view_status_phases_win);
    RUN_TEST(test_pull_progress_view_installed_state);
    RUN_TEST(test_pull_progress_view_failure_not_in_status);
    RUN_TEST(test_pull_progress_view_other_states_none);
    RUN_TEST(test_progress_title_and_connecting_text);
    RUN_TEST(test_progress_status_text_pull_vs_push);
}
