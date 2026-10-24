#include <unity.h>
#include <string.h>
#include "network/pull_logic.h"
#include "system/mbedtls_psram_override.h"

static const PullState kAllStates[] = {PullState::IDLE,        PullState::CHECKING,    PullState::UP_TO_DATE,
                                       PullState::AVAILABLE,   PullState::CHECK_FAILED, PullState::DOWNLOADING,
                                       PullState::VERIFYING,   PullState::INSTALLED,   PullState::FAILED};

static PullPreconditions good_facts() {
    PullPreconditions facts;
    facts.wifi_connected = true;
    facts.internal_free = UPDATE_PULL_MIN_FREE_INTERNAL_BYTES + 1;
    return facts;
}

//------------------------------------------------------------------------------
// State machine
//------------------------------------------------------------------------------
static void test_transition_table_is_exact() {
    // Every allowed pair, written out; everything else must be refused
    struct Pair { PullState from, to; };
    const Pair allowed[] = {
        {PullState::IDLE, PullState::CHECKING},
        {PullState::CHECKING, PullState::UP_TO_DATE},
        {PullState::CHECKING, PullState::AVAILABLE},
        {PullState::CHECKING, PullState::CHECK_FAILED},
        {PullState::UP_TO_DATE, PullState::CHECKING},
        {PullState::UP_TO_DATE, PullState::IDLE},
        {PullState::AVAILABLE, PullState::CHECKING},
        {PullState::AVAILABLE, PullState::DOWNLOADING},
        {PullState::AVAILABLE, PullState::IDLE},
        {PullState::CHECK_FAILED, PullState::CHECKING},
        {PullState::CHECK_FAILED, PullState::IDLE},
        {PullState::DOWNLOADING, PullState::VERIFYING},
        {PullState::DOWNLOADING, PullState::FAILED},
        {PullState::VERIFYING, PullState::INSTALLED},
        {PullState::VERIFYING, PullState::FAILED},
        {PullState::FAILED, PullState::CHECKING},
        {PullState::FAILED, PullState::IDLE},
    };
    for (PullState from : kAllStates) {
        for (PullState to : kAllStates) {
            bool expected = false;
            for (const Pair& p : allowed) expected = expected || (p.from == from && p.to == to);
            TEST_ASSERT_EQUAL_MESSAGE(expected, pull_transition_allowed(from, to), pull_state_name(from));
        }
    }
}

static void test_installed_is_terminal() {
    for (PullState to : kAllStates) TEST_ASSERT_FALSE(pull_transition_allowed(PullState::INSTALLED, to));
}

static void test_running_states() {
    TEST_ASSERT_TRUE(pull_state_is_running(PullState::CHECKING));
    TEST_ASSERT_TRUE(pull_state_is_running(PullState::DOWNLOADING));
    TEST_ASSERT_TRUE(pull_state_is_running(PullState::VERIFYING));
    TEST_ASSERT_FALSE(pull_state_is_running(PullState::IDLE));
    TEST_ASSERT_FALSE(pull_state_is_running(PullState::AVAILABLE));
    TEST_ASSERT_FALSE(pull_state_is_running(PullState::INSTALLED));
    TEST_ASSERT_FALSE(pull_state_is_running(PullState::FAILED));
}

static void test_can_start() {
    TEST_ASSERT_TRUE(pull_can_start_check(PullState::IDLE));
    TEST_ASSERT_TRUE(pull_can_start_check(PullState::UP_TO_DATE));
    TEST_ASSERT_TRUE(pull_can_start_check(PullState::AVAILABLE));
    TEST_ASSERT_TRUE(pull_can_start_check(PullState::CHECK_FAILED));
    TEST_ASSERT_TRUE(pull_can_start_check(PullState::FAILED));
    TEST_ASSERT_FALSE(pull_can_start_check(PullState::CHECKING));
    TEST_ASSERT_FALSE(pull_can_start_check(PullState::DOWNLOADING));
    TEST_ASSERT_FALSE(pull_can_start_check(PullState::VERIFYING));
    TEST_ASSERT_FALSE(pull_can_start_check(PullState::INSTALLED));
    for (PullState s : kAllStates) TEST_ASSERT_EQUAL(s == PullState::AVAILABLE, pull_can_start_install(s));
    // The start transitions exist in the table
    TEST_ASSERT_TRUE(pull_transition_allowed(PullState::AVAILABLE, PullState::DOWNLOADING));
    for (PullState s : kAllStates) {
        if (pull_can_start_check(s)) TEST_ASSERT_TRUE(pull_transition_allowed(s, PullState::CHECKING));
    }
}

static void test_names_and_texts_are_printable() {
    for (PullState s : kAllStates) {
        const char* name = pull_state_name(s);
        TEST_ASSERT_TRUE(name[0] != '\0' && name[0] != '?');
    }
    for (int i = 0; i <= static_cast<int>(PullFailure::INTERNAL); i++) {
        const PullFailure f = static_cast<PullFailure>(i);
        const char* text = pull_failure_text(f);
        if (f == PullFailure::NONE) {
            TEST_ASSERT_EQUAL_STRING("", text);
            continue;
        }
        TEST_ASSERT_TRUE(strlen(text) > 0 && strlen(text) <= 32);
        for (const char* c = text; *c; c++) TEST_ASSERT_TRUE(*c >= 0x20 && *c < 0x7f);
    }
}

//------------------------------------------------------------------------------
// Preconditions
//------------------------------------------------------------------------------
static void test_refusal_all_good() {
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_refusal(PullState::IDLE, false, good_facts()));
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_refusal(PullState::AVAILABLE, true, good_facts()));
}

static void test_refusal_state_first() {
    PullPreconditions bad;  // everything wrong
    bad.update_active = true;
    TEST_ASSERT_EQUAL(PullFailure::BUSY, pull_refusal(PullState::CHECKING, false, bad));
    TEST_ASSERT_EQUAL(PullFailure::BUSY, pull_refusal(PullState::DOWNLOADING, false, bad));
    TEST_ASSERT_EQUAL(PullFailure::BUSY, pull_refusal(PullState::DOWNLOADING, true, bad));
    TEST_ASSERT_EQUAL(PullFailure::BUSY, pull_refusal(PullState::VERIFYING, true, bad));
    TEST_ASSERT_EQUAL(PullFailure::BUSY, pull_refusal(PullState::INSTALLED, true, bad));
    TEST_ASSERT_EQUAL(PullFailure::BUSY, pull_refusal(PullState::INSTALLED, false, bad));
    TEST_ASSERT_EQUAL(PullFailure::NOT_AVAILABLE, pull_refusal(PullState::IDLE, true, bad));
    TEST_ASSERT_EQUAL(PullFailure::NOT_AVAILABLE, pull_refusal(PullState::UP_TO_DATE, true, bad));
    TEST_ASSERT_EQUAL(PullFailure::NOT_AVAILABLE, pull_refusal(PullState::FAILED, true, bad));
}

static void test_refusal_order_for_check() {
    PullPreconditions f = good_facts();
    f.wifi_connected = false;
    f.update_active = true;
    f.grinder_busy = true;
    f.internal_free = 0;
    TEST_ASSERT_EQUAL(PullFailure::NO_WIFI, pull_refusal(PullState::IDLE, false, f));
    f.wifi_connected = true;
    TEST_ASSERT_EQUAL(PullFailure::UPDATE_RUNNING, pull_refusal(PullState::IDLE, false, f));
    f.update_active = false;
    TEST_ASSERT_EQUAL(PullFailure::GRINDER_BUSY, pull_refusal(PullState::IDLE, false, f));
    f.grinder_busy = false;
    TEST_ASSERT_EQUAL(PullFailure::LOW_MEMORY, pull_refusal(PullState::IDLE, false, f));
}

static void test_check_ignores_boot_pending_and_ble() {
    PullPreconditions f = good_facts();
    f.boot_pending = true;
    f.ble_connected = true;
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_refusal(PullState::IDLE, false, f));
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_refusal(PullState::AVAILABLE, false, f));
}

static void test_install_refusal_order() {
    PullPreconditions f = good_facts();
    f.boot_pending = true;
    f.ble_connected = true;
    f.internal_free = 0;
    TEST_ASSERT_EQUAL(PullFailure::VERIFYING_FIRMWARE, pull_refusal(PullState::AVAILABLE, true, f));
    f.boot_pending = false;
    TEST_ASSERT_EQUAL(PullFailure::BLE_CONNECTED, pull_refusal(PullState::AVAILABLE, true, f));
    f.ble_connected = false;
    TEST_ASSERT_EQUAL(PullFailure::LOW_MEMORY, pull_refusal(PullState::AVAILABLE, true, f));
    f.grinder_busy = true;
    TEST_ASSERT_EQUAL(PullFailure::GRINDER_BUSY, pull_refusal(PullState::AVAILABLE, true, f));
    f.update_active = true;
    TEST_ASSERT_EQUAL(PullFailure::UPDATE_RUNNING, pull_refusal(PullState::AVAILABLE, true, f));
    f.wifi_connected = false;
    TEST_ASSERT_EQUAL(PullFailure::NO_WIFI, pull_refusal(PullState::AVAILABLE, true, f));
}

static void test_memory_threshold_boundary() {
    PullPreconditions f = good_facts();
    f.internal_free = UPDATE_PULL_MIN_FREE_INTERNAL_BYTES - 1;
    TEST_ASSERT_EQUAL(PullFailure::LOW_MEMORY, pull_refusal(PullState::IDLE, false, f));
    f.internal_free = UPDATE_PULL_MIN_FREE_INTERNAL_BYTES;
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_refusal(PullState::IDLE, false, f));
}

//------------------------------------------------------------------------------
// Index verdict
//------------------------------------------------------------------------------
static ReleaseInfo info_with(const char* version) {
    ReleaseInfo info;
    strncpy(info.version, version, sizeof(info.version) - 1);
    return info;
}

static void test_index_newer_is_available() {
    const ReleaseInfo info = info_with("1.5.0");
    const PullIndexDecision d = pull_evaluate_index(ReleaseParseResult::OK, &info, "1.4.1-rc.3");
    TEST_ASSERT_EQUAL(PullState::AVAILABLE, d.state);
    TEST_ASSERT_EQUAL(PullFailure::NONE, d.failure);
    TEST_ASSERT_FALSE(d.no_release);
}

static void test_index_equal_or_older_is_up_to_date() {
    ReleaseInfo same = info_with("1.4.1-rc.3");
    TEST_ASSERT_EQUAL(PullState::UP_TO_DATE, pull_evaluate_index(ReleaseParseResult::OK, &same, "1.4.1-rc.3").state);
    ReleaseInfo older = info_with("1.4.0");
    TEST_ASSERT_EQUAL(PullState::UP_TO_DATE, pull_evaluate_index(ReleaseParseResult::OK, &older, "1.4.1-rc.3").state);
    // An installed rc of the next version is never offered the older stable
    ReleaseInfo stable = info_with("1.4.0");
    TEST_ASSERT_EQUAL(PullState::UP_TO_DATE, pull_evaluate_index(ReleaseParseResult::OK, &stable, "1.5.0-rc.1").state);
    // But the final release of that rc is offered
    ReleaseInfo final_release = info_with("1.5.0");
    TEST_ASSERT_EQUAL(PullState::AVAILABLE, pull_evaluate_index(ReleaseParseResult::OK, &final_release, "1.5.0-rc.1").state);
}

static void test_index_unparsable_installed_version_offers_nothing() {
    const ReleaseInfo info = info_with("1.5.0");
    TEST_ASSERT_EQUAL(PullState::UP_TO_DATE, pull_evaluate_index(ReleaseParseResult::OK, &info, "dev").state);
    TEST_ASSERT_EQUAL(PullState::UP_TO_DATE, pull_evaluate_index(ReleaseParseResult::OK, &info, "").state);
}

static void test_index_none_is_up_to_date_without_release() {
    const PullIndexDecision d = pull_evaluate_index(ReleaseParseResult::NONE, nullptr, "1.4.1");
    TEST_ASSERT_EQUAL(PullState::UP_TO_DATE, d.state);
    TEST_ASSERT_TRUE(d.no_release);
    TEST_ASSERT_EQUAL(PullFailure::NONE, d.failure);
}

static void test_index_errors_fail_the_check() {
    const ReleaseParseResult errors[] = {
        ReleaseParseResult::BODY_EMPTY, ReleaseParseResult::BODY_TOO_LARGE, ReleaseParseResult::MALFORMED,
        ReleaseParseResult::BAD_SCHEMA, ReleaseParseResult::BAD_CHANNEL,    ReleaseParseResult::MISSING_FIELD,
        ReleaseParseResult::BAD_VERSION, ReleaseParseResult::BAD_PUBLISHED, ReleaseParseResult::BAD_SIZE,
        ReleaseParseResult::BAD_SHA,    ReleaseParseResult::BAD_URL};
    const ReleaseInfo info = info_with("9.9.9");
    for (ReleaseParseResult r : errors) {
        const PullIndexDecision d = pull_evaluate_index(r, &info, "1.0.0");
        TEST_ASSERT_EQUAL(PullState::CHECK_FAILED, d.state);
        TEST_ASSERT_EQUAL(PullFailure::INDEX_INVALID, d.failure);
        TEST_ASSERT_FALSE(d.no_release);
    }
    // A null info with OK is treated as a failure, not dereferenced
    TEST_ASSERT_EQUAL(PullState::CHECK_FAILED, pull_evaluate_index(ReleaseParseResult::OK, nullptr, "1.0.0").state);
}

//------------------------------------------------------------------------------
// HTTP response checks
//------------------------------------------------------------------------------
static void test_url_scheme() {
    TEST_ASSERT_TRUE(pull_url_scheme_allowed("https://example.org/a.bin", false));
    TEST_ASSERT_TRUE(pull_url_scheme_allowed("https://example.org/a.bin", true));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed("http://example.org/a.bin", false));
    TEST_ASSERT_TRUE(pull_url_scheme_allowed("http://192.168.1.20:8080/a.bin", true));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed("ftp://example.org/a.bin", true));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed("HTTPS://example.org/", false));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed("https://", true));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed("http://", true));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed("", true));
    TEST_ASSERT_FALSE(pull_url_scheme_allowed(nullptr, true));
}

static void test_response_check() {
    const int64_t size = 2240000;
    TEST_ASSERT_EQUAL(PullHttpCheck::OK, pull_check_response(200, size, size, size));
    TEST_ASSERT_EQUAL(PullHttpCheck::OK, pull_check_response(200, 1500, -1, 2048));      // index: any length up to the cap
    TEST_ASSERT_EQUAL(PullHttpCheck::OK, pull_check_response(200, 2048, -1, 2048));
    TEST_ASSERT_EQUAL(PullHttpCheck::BAD_STATUS, pull_check_response(404, size, size, size));
    TEST_ASSERT_EQUAL(PullHttpCheck::BAD_STATUS, pull_check_response(301, size, size, size));
    TEST_ASSERT_EQUAL(PullHttpCheck::BAD_STATUS, pull_check_response(206, size, size, size));
    TEST_ASSERT_EQUAL(PullHttpCheck::BAD_STATUS, pull_check_response(0, size, size, size));
    TEST_ASSERT_EQUAL(PullHttpCheck::NO_LENGTH, pull_check_response(200, 0, size, size));   // chunked / absent
    TEST_ASSERT_EQUAL(PullHttpCheck::NO_LENGTH, pull_check_response(200, -1, size, size));
    TEST_ASSERT_EQUAL(PullHttpCheck::TOO_LARGE, pull_check_response(200, 2049, -1, 2048));
    TEST_ASSERT_EQUAL(PullHttpCheck::LENGTH_MISMATCH, pull_check_response(200, size + 1, size, size + 100));
    TEST_ASSERT_EQUAL(PullHttpCheck::LENGTH_MISMATCH, pull_check_response(200, size - 1, size, size));
}

static void test_response_check_failure_mapping() {
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_failure_for_http_check(PullHttpCheck::OK, 200));
    TEST_ASSERT_EQUAL(PullFailure::NOT_FOUND, pull_failure_for_http_check(PullHttpCheck::BAD_STATUS, 404));
    TEST_ASSERT_EQUAL(PullFailure::SERVER_ERROR, pull_failure_for_http_check(PullHttpCheck::BAD_STATUS, 500));
    TEST_ASSERT_EQUAL(PullFailure::SERVER_ERROR, pull_failure_for_http_check(PullHttpCheck::BAD_STATUS, 302));
    TEST_ASSERT_EQUAL(PullFailure::BAD_REPLY, pull_failure_for_http_check(PullHttpCheck::NO_LENGTH, 200));
    TEST_ASSERT_EQUAL(PullFailure::BAD_REPLY, pull_failure_for_http_check(PullHttpCheck::LENGTH_MISMATCH, 200));
    TEST_ASSERT_EQUAL(PullFailure::REPLY_TOO_LARGE, pull_failure_for_http_check(PullHttpCheck::TOO_LARGE, 200));
}

static void test_read_classification() {
    TEST_ASSERT_EQUAL(PullReadOutcome::DATA, pull_classify_read(4096, 0, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::DATA, pull_classify_read(10000, 0, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::OVERRUN, pull_classify_read(10001, 0, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::OVERRUN, pull_classify_read(2, 9999, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::OVERRUN, pull_classify_read(100, 0xFFFFFFF0u, 0xFFFFFFFFu));  // 32-bit sum must not wrap
    TEST_ASSERT_EQUAL(PullReadOutcome::DONE, pull_classify_read(0, 10000, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::TRUNCATED, pull_classify_read(0, 9999, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::TRUNCATED, pull_classify_read(0, 0, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::TIMEOUT, pull_classify_read(-0x7007, 100, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::READ_ERROR, pull_classify_read(-1, 100, 10000));
    TEST_ASSERT_EQUAL(PullReadOutcome::READ_ERROR, pull_classify_read(-0x7005, 100, 10000));
}

static void test_download_deadline() {
    const uint32_t max = UPDATE_PULL_DOWNLOAD_MAX_MS;
    TEST_ASSERT_FALSE(pull_download_deadline_exceeded(1000, 1000, max));
    TEST_ASSERT_FALSE(pull_download_deadline_exceeded(1000, 1000 + max, max));
    TEST_ASSERT_TRUE(pull_download_deadline_exceeded(1000, 1001 + max, max));
    // millis() wrap in the middle of a download
    TEST_ASSERT_FALSE(pull_download_deadline_exceeded(0xFFFFFF00u, 0x100u, max));
    TEST_ASSERT_TRUE(pull_download_deadline_exceeded(0xFFFFFF00u, 0x100u + max, max));
}

static void test_read_failure_mapping() {
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_failure_for_read(PullReadOutcome::DATA));
    TEST_ASSERT_EQUAL(PullFailure::NONE, pull_failure_for_read(PullReadOutcome::DONE));
    TEST_ASSERT_EQUAL(PullFailure::DOWNLOAD_INTERRUPTED, pull_failure_for_read(PullReadOutcome::TRUNCATED));
    TEST_ASSERT_EQUAL(PullFailure::DOWNLOAD_STALLED, pull_failure_for_read(PullReadOutcome::TIMEOUT));
    TEST_ASSERT_EQUAL(PullFailure::CONNECTION_LOST, pull_failure_for_read(PullReadOutcome::READ_ERROR));
    TEST_ASSERT_EQUAL(PullFailure::BAD_REPLY, pull_failure_for_read(PullReadOutcome::OVERRUN));
}

//------------------------------------------------------------------------------
// TlsPsramScope counter
//------------------------------------------------------------------------------
static void test_tls_scope_nests_and_releases() {
#if !UPDATE_PULL_TLS_PSRAM_UNSCOPED
    TEST_ASSERT_FALSE(tls_psram_scope_active());
    {
        TlsPsramScope outer;
        TEST_ASSERT_TRUE(tls_psram_scope_active());
        {
            TlsPsramScope inner;
            TEST_ASSERT_TRUE(tls_psram_scope_active());
        }
        TEST_ASSERT_TRUE(tls_psram_scope_active());
    }
    TEST_ASSERT_FALSE(tls_psram_scope_active());
#else
    TEST_ASSERT_TRUE(tls_psram_scope_active());
#endif
}

void run_pull_logic_tests() {
    RUN_TEST(test_transition_table_is_exact);
    RUN_TEST(test_installed_is_terminal);
    RUN_TEST(test_running_states);
    RUN_TEST(test_can_start);
    RUN_TEST(test_names_and_texts_are_printable);
    RUN_TEST(test_refusal_all_good);
    RUN_TEST(test_refusal_state_first);
    RUN_TEST(test_refusal_order_for_check);
    RUN_TEST(test_check_ignores_boot_pending_and_ble);
    RUN_TEST(test_install_refusal_order);
    RUN_TEST(test_memory_threshold_boundary);
    RUN_TEST(test_index_newer_is_available);
    RUN_TEST(test_index_equal_or_older_is_up_to_date);
    RUN_TEST(test_index_unparsable_installed_version_offers_nothing);
    RUN_TEST(test_index_none_is_up_to_date_without_release);
    RUN_TEST(test_index_errors_fail_the_check);
    RUN_TEST(test_url_scheme);
    RUN_TEST(test_response_check);
    RUN_TEST(test_response_check_failure_mapping);
    RUN_TEST(test_read_classification);
    RUN_TEST(test_download_deadline);
    RUN_TEST(test_read_failure_mapping);
    RUN_TEST(test_tls_scope_nests_and_releases);
}
