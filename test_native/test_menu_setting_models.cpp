#include <unity.h>
#include <math.h>
#include <string.h>
#include "system/menu_setting_models.h"

static bool bits_equal(float a, float b) { return memcmp(&a, &b, sizeof(float)) == 0; }

static void test_brightness_clamp_percent() {
    TEST_ASSERT_EQUAL_INT(15, brightness_clamp_percent(0));
    TEST_ASSERT_EQUAL_INT(15, brightness_clamp_percent(14));
    TEST_ASSERT_EQUAL_INT(15, brightness_clamp_percent(15));
    TEST_ASSERT_EQUAL_INT(16, brightness_clamp_percent(16));
    TEST_ASSERT_EQUAL_INT(100, brightness_clamp_percent(100));
}

static void test_brightness_percent_truncation_is_pinned() {
    // (int)(f * 100) truncates; on the host 0.57f * 100 gives 57 and 0.99f * 100 gives 99 (literals pinned)
    TEST_ASSERT_EQUAL_INT(15, brightness_percent_from_fraction(0.15f));
    TEST_ASSERT_EQUAL_INT(57, brightness_percent_from_fraction(0.57f));
    TEST_ASSERT_EQUAL_INT(99, brightness_percent_from_fraction(0.99f));
    TEST_ASSERT_EQUAL_INT(100, brightness_percent_from_fraction(1.0f));
    TEST_ASSERT_EQUAL_INT(15, brightness_percent_from_fraction(0.0f));
    TEST_ASSERT_EQUAL_INT(15, brightness_percent_from_fraction(NAN));
}

static void test_brightness_fraction_round_trip_and_floor() {
    TEST_ASSERT_TRUE(bits_equal(0.15f, brightness_fraction_from_percent(15)));
    TEST_ASSERT_TRUE(bits_equal(0.15f, brightness_fraction_from_percent(3)));
    TEST_ASSERT_TRUE(bits_equal(1.0f, brightness_fraction_from_percent(100)));
    TEST_ASSERT_TRUE(bits_equal(57 / 100.0f, brightness_fraction_from_percent(57)));
    TEST_ASSERT_TRUE(bits_equal(0.15f, brightness_fraction_floor(0.10f)));
    TEST_ASSERT_TRUE(bits_equal(15 / 100.0f, brightness_fraction_floor(0.10f)));
    TEST_ASSERT_TRUE(bits_equal(0.5f, brightness_fraction_floor(0.5f)));
    TEST_ASSERT_TRUE(bits_equal(0.15f, brightness_fraction_floor(0.15f)));
}

static void test_brightness_labels() {
    char buf[32];
    TEST_ASSERT_EQUAL_STRING("Brightness: 57%", brightness_label(buf, sizeof(buf), 57, false));
    TEST_ASSERT_EQUAL_STRING("Dimmed: 35%", brightness_label(buf, sizeof(buf), 35, true));
    TEST_ASSERT_EQUAL_STRING("Bri", brightness_label(buf, 4, 57, false));
    TEST_ASSERT_NULL(brightness_label(nullptr, 4, 57, false));
}

static void test_purge_slider_range() {
    TEST_ASSERT_EQUAL_INT(1, purge_slider_min());
    TEST_ASSERT_EQUAL_INT(25, purge_slider_max());
}

static void test_purge_amount_from_slider() {
    TEST_ASSERT_TRUE(bits_equal(0.1f, purge_amount_from_slider(0)));
    TEST_ASSERT_TRUE(bits_equal(0.1f, purge_amount_from_slider(1)));
    TEST_ASSERT_TRUE(bits_equal(17 / 10.0f, purge_amount_from_slider(17)));
    TEST_ASSERT_TRUE(bits_equal(2.5f, purge_amount_from_slider(25)));
    TEST_ASSERT_TRUE(bits_equal(2.5f, purge_amount_from_slider(26)));
}

static void test_purge_slider_from_amount() {
    TEST_ASSERT_EQUAL_INT(1, purge_slider_from_amount(0.0f));
    TEST_ASSERT_EQUAL_INT(1, purge_slider_from_amount(0.1f));
    TEST_ASSERT_EQUAL_INT(17, purge_slider_from_amount(1.7f));
    TEST_ASSERT_EQUAL_INT(25, purge_slider_from_amount(2.5f));
    TEST_ASSERT_EQUAL_INT(25, purge_slider_from_amount(9.9f));
    TEST_ASSERT_EQUAL_INT(1, purge_slider_from_amount(NAN));
}

static void test_purge_label() {
    char buf[16];
    TEST_ASSERT_EQUAL_STRING("Amount: 1.7g", purge_label(buf, sizeof(buf), 1.7f));
    TEST_ASSERT_EQUAL_STRING("Amount: 2.5g", purge_label(buf, sizeof(buf), 9.0f));
    TEST_ASSERT_EQUAL_STRING("Amount: 0.1g", purge_label(buf, sizeof(buf), 0.0f));
}

static void test_freshness_steps_round_trip() {
    const float steps[9] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 8.0f, 12.0f, 24.0f, 48.0f};
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_INT(i, freshness_index_from_hours(steps[i]));
        TEST_ASSERT_TRUE(bits_equal(steps[i], freshness_hours_from_index(i)));
    }
}

static void test_freshness_index_from_hours_edges() {
    TEST_ASSERT_EQUAL_INT(5, freshness_index_from_hours(5.0f));   // not within 0.1 of any step -> default
    TEST_ASSERT_EQUAL_INT(0, freshness_index_from_hours(0.55f));
    TEST_ASSERT_EQUAL_INT(1, freshness_index_from_hours(1.05f));
    TEST_ASSERT_EQUAL_INT(5, freshness_index_from_hours(6.0f));
    TEST_ASSERT_EQUAL_INT(5, freshness_index_from_hours(NAN));
}

static void test_freshness_hours_from_index_clamps() {
    TEST_ASSERT_TRUE(bits_equal(0.5f, freshness_hours_from_index(-3)));
    TEST_ASSERT_TRUE(bits_equal(48.0f, freshness_hours_from_index(99)));
}

static void test_freshness_labels() {
    char buf[24];
    TEST_ASSERT_EQUAL_STRING("Freshness: 0.5h", freshness_label(buf, sizeof(buf), 0.5f));
    TEST_ASSERT_EQUAL_STRING("Freshness: 1h", freshness_label(buf, sizeof(buf), 1.0f));
    TEST_ASSERT_EQUAL_STRING("Freshness: 48h", freshness_label(buf, sizeof(buf), 48.0f));
    TEST_ASSERT_EQUAL_STRING("Freshness: 5h", freshness_label(buf, sizeof(buf), 5.0f));
}

static void test_grind_mode_radio_index() {
    TEST_ASSERT_EQUAL_INT(0, grind_mode_radio_index(0));
    TEST_ASSERT_EQUAL_INT(1, grind_mode_radio_index(1));
    TEST_ASSERT_EQUAL_INT(0, grind_mode_radio_index(2));
    TEST_ASSERT_EQUAL_INT(0, grind_mode_radio_index(-1));
}

void run_menu_setting_models_tests() {
    RUN_TEST(test_brightness_clamp_percent);
    RUN_TEST(test_brightness_percent_truncation_is_pinned);
    RUN_TEST(test_brightness_fraction_round_trip_and_floor);
    RUN_TEST(test_brightness_labels);
    RUN_TEST(test_purge_slider_range);
    RUN_TEST(test_purge_amount_from_slider);
    RUN_TEST(test_purge_slider_from_amount);
    RUN_TEST(test_purge_label);
    RUN_TEST(test_freshness_steps_round_trip);
    RUN_TEST(test_freshness_index_from_hours_edges);
    RUN_TEST(test_freshness_hours_from_index_clamps);
    RUN_TEST(test_freshness_labels);
    RUN_TEST(test_grind_mode_radio_index);
}
