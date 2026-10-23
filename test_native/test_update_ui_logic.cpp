#include <unity.h>
#include <string.h>
#include "network/update_ui_logic.h"

static void test_prompt_action_idle_and_show() {
    TEST_ASSERT_EQUAL(PromptAction::NONE, update_prompt_action(false, false, true, true));
    TEST_ASSERT_EQUAL(PromptAction::NONE, update_prompt_action(false, false, false, false));
    TEST_ASSERT_EQUAL(PromptAction::SHOW, update_prompt_action(true, false, false, true));
    TEST_ASSERT_EQUAL(PromptAction::REFUSE_BUSY, update_prompt_action(true, false, false, false));
}

static void test_prompt_action_dialog_up() {
    // Request still the same and pending: leave the dialog alone, whatever the screen rule says now
    TEST_ASSERT_EQUAL(PromptAction::NONE, update_prompt_action(true, true, true, true));
    TEST_ASSERT_EQUAL(PromptAction::NONE, update_prompt_action(true, true, true, false));
    // Expired / cancelled / answered elsewhere
    TEST_ASSERT_EQUAL(PromptAction::DISMISS, update_prompt_action(false, true, true, true));
    TEST_ASSERT_EQUAL(PromptAction::DISMISS, update_prompt_action(false, true, false, false));
    // A newer request replaced the one the dialog shows
    TEST_ASSERT_EQUAL(PromptAction::DISMISS, update_prompt_action(true, true, false, true));
}

static void test_state_allows_prompt() {
    TEST_ASSERT_TRUE(update_ui_state_allows_prompt(true, false));
    TEST_ASSERT_TRUE(update_ui_state_allows_prompt(false, true));
    TEST_ASSERT_FALSE(update_ui_state_allows_prompt(false, false));
}

static void test_progress_percent() {
    TEST_ASSERT_EQUAL_INT(0, update_progress_percent(0, 0));
    TEST_ASSERT_EQUAL_INT(0, update_progress_percent(500, 0));
    TEST_ASSERT_EQUAL_INT(0, update_progress_percent(0, 1000));
    TEST_ASSERT_EQUAL_INT(50, update_progress_percent(500, 1000));
    TEST_ASSERT_EQUAL_INT(99, update_progress_percent(999, 1000));
    TEST_ASSERT_EQUAL_INT(100, update_progress_percent(1000, 1000));
    TEST_ASSERT_EQUAL_INT(100, update_progress_percent(2000, 1000));
    // No 32-bit overflow near the top of the range
    TEST_ASSERT_EQUAL_INT(50, update_progress_percent(2147483648u, 4294967295u));
}

static void test_status_text_phases() {
    char buf[UPDATE_UI_STATUS_BUF];
    TEST_ASSERT_EQUAL_STRING("Receiving update...", update_progress_status_text(UpdatePhase::RECEIVING, nullptr, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Verifying...", update_progress_status_text(UpdatePhase::VERIFYING, nullptr, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Installed. Restarting...", update_progress_status_text(UpdatePhase::INSTALLED, nullptr, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", update_progress_status_text(UpdatePhase::IDLE, "x", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", update_progress_status_text(UpdatePhase::AWAITING_APPROVAL, "x", buf, sizeof(buf)));
}

static void test_status_text_failed() {
    char buf[UPDATE_UI_STATUS_BUF];
    TEST_ASSERT_EQUAL_STRING("Failed: Checksum mism...",
                             update_progress_status_text(UpdatePhase::FAILED, "Checksum mismatch", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Update failed", update_progress_status_text(UpdatePhase::FAILED, "", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Update failed", update_progress_status_text(UpdatePhase::FAILED, nullptr, buf, sizeof(buf)));
    // Control and non-ASCII bytes never reach the screen
    TEST_ASSERT_EQUAL_STRING("Failed: a?b?c",
                             update_progress_status_text(UpdatePhase::FAILED, "a\nb\xC3" "c", buf, sizeof(buf)));
}

static void test_status_text_failed_long_reason_is_shortened() {
    char buf[UPDATE_UI_STATUS_BUF];
    char exact[UPDATE_UI_REASON_MAX + 1];
    memset(exact, 'e', UPDATE_UI_REASON_MAX);
    exact[UPDATE_UI_REASON_MAX] = '\0';
    update_progress_status_text(UpdatePhase::FAILED, exact, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(8 + UPDATE_UI_REASON_MAX, strlen(buf));
    TEST_ASSERT_NULL(strstr(buf, "..."));  // exactly at the limit: unchanged

    char longer[100];
    memset(longer, 'l', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = '\0';
    update_progress_status_text(UpdatePhase::FAILED, longer, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(8 + UPDATE_UI_REASON_MAX, strlen(buf));
    TEST_ASSERT_EQUAL_STRING("...", buf + strlen(buf) - 3);
}

static void test_status_text_small_buffer_is_terminated() {
    char buf[8];
    memset(buf, 'x', sizeof(buf));
    update_progress_status_text(UpdatePhase::FAILED, "Checksum mismatch", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
    TEST_ASSERT_EQUAL_STRING("Failed:", buf);
    update_progress_status_text(UpdatePhase::RECEIVING, nullptr, nullptr, 0);  // must not crash
    update_progress_status_text(UpdatePhase::RECEIVING, nullptr, buf, 0);
}

static void test_listening_text() {
    char buf[96];
    TEST_ASSERT_EQUAL_STRING("Listening:\ngrindbyweight.local", update_listening_text("grindbyweight", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Listening:\n.local", update_listening_text(nullptr, buf, sizeof(buf)));
    char host[100];
    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    update_listening_text(host, buf, sizeof(buf));
    TEST_ASSERT_LESS_THAN(96, strlen(buf));
    TEST_ASSERT_EQUAL_STRING(".local", buf + strlen(buf) - 6);
}

void run_update_ui_logic_tests() {
    RUN_TEST(test_prompt_action_idle_and_show);
    RUN_TEST(test_prompt_action_dialog_up);
    RUN_TEST(test_state_allows_prompt);
    RUN_TEST(test_progress_percent);
    RUN_TEST(test_status_text_phases);
    RUN_TEST(test_status_text_failed);
    RUN_TEST(test_status_text_failed_long_reason_is_shortened);
    RUN_TEST(test_status_text_small_buffer_is_terminated);
    RUN_TEST(test_listening_text);
}
