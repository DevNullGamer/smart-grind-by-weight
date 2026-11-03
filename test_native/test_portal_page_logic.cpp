#include <unity.h>
#include <string.h>
#include "network/portal_page_logic.h"

static void test_budget_boundaries() {
    TEST_ASSERT_FALSE(portal_page_within_budget(0));
    TEST_ASSERT_TRUE(portal_page_within_budget(1));
    TEST_ASSERT_TRUE(portal_page_within_budget(PORTAL_PAGE_BUDGET_BYTES));
    TEST_ASSERT_FALSE(portal_page_within_budget(PORTAL_PAGE_BUDGET_BYTES + 1));
    TEST_ASSERT_EQUAL(64 * 1024, PORTAL_PAGE_BUDGET_BYTES);
}

static void test_etag_format_and_keying() {
    char a[PORTAL_PAGE_ETAG_MAX_LEN];
    char b[PORTAL_PAGE_ETAG_MAX_LEN];
    TEST_ASSERT_EQUAL(15, portal_page_etag(495, 0xdeadbeef, a, sizeof(a)));
    TEST_ASSERT_EQUAL_STRING("\"b495-deadbeef\"", a);
    portal_page_etag(496, 0xdeadbeef, b, sizeof(b));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);  // a new build number revalidates
    portal_page_etag(495, 0xdeadbeee, b, sizeof(b));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);  // different content too
    portal_page_etag(7, 0x1, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("\"b7-00000001\"", b);
}

static void test_etag_fits_declared_buffer_for_extreme_values() {
    char out[PORTAL_PAGE_ETAG_MAX_LEN];
    TEST_ASSERT_GREATER_THAN(0, portal_page_etag(INT32_MIN, 0xffffffffu, out, sizeof(out)));
    TEST_ASSERT_LESS_THAN(PORTAL_PAGE_ETAG_MAX_LEN, strlen(out));
}

static void test_etag_small_or_null_buffer() {
    char tiny[8];
    memset(tiny, 'x', sizeof(tiny));
    TEST_ASSERT_EQUAL(0, portal_page_etag(495, 1, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);
    TEST_ASSERT_EQUAL(0, portal_page_etag(495, 1, tiny, 0));
    TEST_ASSERT_EQUAL(0, portal_page_etag(495, 1, nullptr, 8));
}

static void test_if_none_match() {
    const char* tag = "\"b495-deadbeef\"";
    TEST_ASSERT_TRUE(portal_etag_matches("\"b495-deadbeef\"", tag));
    TEST_ASSERT_TRUE(portal_etag_matches("W/\"b495-deadbeef\"", tag));
    TEST_ASSERT_TRUE(portal_etag_matches("\"x\", \"b495-deadbeef\" ,\"y\"", tag));
    TEST_ASSERT_TRUE(portal_etag_matches("*", tag));
    TEST_ASSERT_FALSE(portal_etag_matches("\"b494-deadbeef\"", tag));
    TEST_ASSERT_FALSE(portal_etag_matches("\"b495-deadbee\"", tag));
    TEST_ASSERT_FALSE(portal_etag_matches("b495-deadbeef", tag));  // unquoted is not the tag
    TEST_ASSERT_FALSE(portal_etag_matches("", tag));
    TEST_ASSERT_FALSE(portal_etag_matches(" , ,", tag));
    TEST_ASSERT_FALSE(portal_etag_matches(nullptr, tag));
    TEST_ASSERT_FALSE(portal_etag_matches("\"b495-deadbeef\"", nullptr));
    TEST_ASSERT_FALSE(portal_etag_matches("\"b495-deadbeef\"", ""));
    TEST_ASSERT_FALSE(portal_etag_matches("**", tag));
}

void run_portal_page_logic_tests() {
    RUN_TEST(test_budget_boundaries);
    RUN_TEST(test_etag_format_and_keying);
    RUN_TEST(test_etag_fits_declared_buffer_for_extreme_values);
    RUN_TEST(test_etag_small_or_null_buffer);
    RUN_TEST(test_if_none_match);
}
