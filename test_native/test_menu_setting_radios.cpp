#include <unity.h>
#include <string.h>
#include "system/menu_setting_table.h"

namespace {

// What shipped firmware shows, do not edit.
struct Golden {
    RadioSetting id;
    const char* labels[RADIO_MAX_OPTIONS];
    int count, initial, width, height;
};
const Golden kGolden[] = {
    {RadioSetting::GRIND_MODE, {"Weight", "Time", nullptr}, 2, 0, 135, 100},
    {RadioSetting::PURGE_MODE, {"Keep", "Remove", nullptr}, 2, 1, 135, 100},
    {RadioSetting::BOARD_OVERRIDE, {"Auto", "V1", "V2"}, 3, 0, -1, 100},
};

void test_table_covers_every_radio_in_order() {
    TEST_ASSERT_EQUAL(sizeof(kGolden) / sizeof(kGolden[0]), RADIO_SETTING_COUNT);
    for (size_t i = 0; i < RADIO_SETTING_COUNT; i++) {
        TEST_ASSERT_EQUAL(i, static_cast<size_t>(kGolden[i].id));
    }
}

void test_labels_count_initial_and_size() {
    for (const Golden& g : kGolden) {
        const RadioSettingDef& d = radio_setting_def(g.id);
        TEST_ASSERT_EQUAL_INT(g.count, d.count);
        TEST_ASSERT_EQUAL_INT(g.initial, d.initial_index);
        TEST_ASSERT_EQUAL_INT(g.width, d.width);
        TEST_ASSERT_EQUAL_INT(g.height, d.height);
        for (int i = 0; i < g.count; i++) {
            TEST_ASSERT_NOT_NULL(d.labels[i]);
            TEST_ASSERT_EQUAL_STRING(g.labels[i], d.labels[i]);
        }
        TEST_ASSERT_TRUE(d.initial_index >= 0 && d.initial_index < d.count);
        TEST_ASSERT_TRUE(d.count <= static_cast<int>(RADIO_MAX_OPTIONS));
    }
}

}  // namespace

void run_menu_setting_radios_tests() {
    RUN_TEST(test_table_covers_every_radio_in_order);
    RUN_TEST(test_labels_count_initial_and_size);
}
