#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "system/menu_setting_table.h"
#include "system/settings_store.h"
#include "system/ui_settings.h"

namespace {

// In-memory store that logs every put as "float <ns>/<key>" (the sliders only use float keys).
class FloatStore : public SettingsStore {
public:
    struct FloatSlot {
        std::string ns, key;
        float value;
    };
    std::vector<FloatSlot> floats;
    std::vector<std::string> puts;

    bool get_bool(const char*, const char*, bool d) override { return d; }
    bool put_bool(const char*, const char*, bool) override { puts.push_back("bool"); return false; }
    int32_t get_int(const char*, const char*, int32_t d) override { return d; }
    bool put_int(const char*, const char*, int32_t) override { puts.push_back("int"); return false; }
    float get_float(const char* ns, const char* key, float d) override {
        for (auto& s : floats) if (s.ns == ns && s.key == key) return s.value;
        return d;
    }
    bool put_float(const char* ns, const char* key, float v) override {
        puts.push_back(std::string("float ") + ns + "/" + key);
        for (auto& s : floats) {
            if (s.ns == ns && s.key == key) { s.value = v; return true; }
        }
        floats.push_back({ns, key, v});
        return true;
    }
};

// What shipped firmware shows and writes, do not edit.
struct Golden {
    SliderSetting id;
    const char* row_label;
    uint32_t color;
    int min, max;
    const char* put;  // "float <ns>/<key>"
    SliderAfter after;
    bool live;
};
const Golden kGolden[] = {
    {SliderSetting::BRIGHTNESS_NORMAL, "Brightness", 0x00AAFF, 0, 100, "float brightness/normal",
     SliderAfter::NONE, true},
    {SliderSetting::BRIGHTNESS_SCREENSAVER, "Screensaver", 0xCC8800, 0, 100, "float brightness/screensaver",
     SliderAfter::RESTORE_NORMAL_BRIGHTNESS, true},
    {SliderSetting::PURGE_AMOUNT, "Amount", 0x00AAFF, 1, 25, "float grinder/grinder_amt_g",
     SliderAfter::NONE, false},
    {SliderSetting::FRESHNESS, "Freshness", 0x00AAFF, 0, 8, "float grinder/freshness_hrs",
     SliderAfter::NONE, false},
};
const size_t kRows = sizeof(kGolden) / sizeof(kGolden[0]);

// slider position -> label text and stored value
struct Point {
    SliderSetting id;
    int pos;
    const char* label;
    float stored;
};
const Point kPoints[] = {
    {SliderSetting::BRIGHTNESS_NORMAL, 5, "Brightness: 15%", 0.15f},  // clamped, below the display minimum
    {SliderSetting::BRIGHTNESS_NORMAL, 60, "Brightness: 60%", 0.6f},
    {SliderSetting::BRIGHTNESS_NORMAL, 100, "Brightness: 100%", 1.0f},
    {SliderSetting::BRIGHTNESS_SCREENSAVER, 5, "Dimmed: 15%", 0.15f},
    {SliderSetting::BRIGHTNESS_SCREENSAVER, 60, "Dimmed: 60%", 0.6f},
    {SliderSetting::BRIGHTNESS_SCREENSAVER, 100, "Dimmed: 100%", 1.0f},
    {SliderSetting::PURGE_AMOUNT, 1, "Amount: 0.1g", 0.1f},
    {SliderSetting::PURGE_AMOUNT, 13, "Amount: 1.3g", 1.3f},
    {SliderSetting::PURGE_AMOUNT, 25, "Amount: 2.5g", 2.5f},
    {SliderSetting::FRESHNESS, 0, "Freshness: 0.5h", 0.5f},
    {SliderSetting::FRESHNESS, 3, "Freshness: 3h", 3.0f},
    {SliderSetting::FRESHNESS, 8, "Freshness: 48h", 48.0f},
};

}  // namespace

static void test_table_covers_every_slider_in_order() {
    TEST_ASSERT_EQUAL_UINT(kRows, SLIDER_SETTING_COUNT);
    for (size_t i = 0; i < kRows; i++) {
        TEST_ASSERT_EQUAL_UINT(i, static_cast<size_t>(kGolden[i].id));
    }
}

// Every slider has every hook; the only documented exception is live_brightness (brightness sliders only)
static void test_hook_table_is_complete() {
    for (const Golden& g : kGolden) {
        const SliderSettingDef& d = slider_setting_def(g.id);
        TEST_ASSERT_NOT_NULL(d.row_label);
        TEST_ASSERT_NOT_NULL(d.load_pos);
        TEST_ASSERT_NOT_NULL(d.clamp_pos);
        TEST_ASSERT_NOT_NULL(d.label);
        TEST_ASSERT_NOT_NULL(d.store);
        TEST_ASSERT_NOT_NULL(d.log_name);
        TEST_ASSERT_EQUAL(g.live, d.live_brightness != nullptr);
        TEST_ASSERT_TRUE(d.after == g.after);
    }
}

static void test_row_label_color_and_range() {
    for (const Golden& g : kGolden) {
        const SliderSettingDef& d = slider_setting_def(g.id);
        TEST_ASSERT_EQUAL_STRING(g.row_label, d.row_label);
        TEST_ASSERT_EQUAL_UINT32(g.color, d.color);
        TEST_ASSERT_EQUAL_INT(g.min, d.min);
        TEST_ASSERT_EQUAL_INT(g.max, d.max);
    }
}

static void test_label_text_at_each_position() {
    char buf[32];
    for (const Point& p : kPoints) {
        const SliderSettingDef& d = slider_setting_def(p.id);
        TEST_ASSERT_EQUAL_STRING(p.label, d.label(buf, sizeof(buf), d.clamp_pos(p.pos)));
    }
}

static void test_store_writes_one_float_put_of_the_legacy_value() {
    for (const Point& p : kPoints) {
        FloatStore store;
        UiSettings s(store);
        const SliderSettingDef& d = slider_setting_def(p.id);
        TEST_ASSERT_TRUE(d.store(s, d.clamp_pos(p.pos)));
        const char* expect_put = nullptr;
        for (const Golden& g : kGolden) {
            if (g.id == p.id) expect_put = g.put;
        }
        TEST_ASSERT_EQUAL_UINT(1, store.puts.size());
        TEST_ASSERT_EQUAL_STRING(expect_put, store.puts[0].c_str());
        TEST_ASSERT_EQUAL_UINT(1, store.floats.size());
        TEST_ASSERT_EQUAL_FLOAT(p.stored, store.floats[0].value);
    }
}

static void test_load_pos_defaults() {
    FloatStore store;
    UiSettings s(store);
    TEST_ASSERT_EQUAL_INT(100, slider_setting_def(SliderSetting::BRIGHTNESS_NORMAL).load_pos(s));
    TEST_ASSERT_EQUAL_INT(35, slider_setting_def(SliderSetting::BRIGHTNESS_SCREENSAVER).load_pos(s));
    TEST_ASSERT_EQUAL_INT(10, slider_setting_def(SliderSetting::PURGE_AMOUNT).load_pos(s));
    TEST_ASSERT_EQUAL_INT(5, slider_setting_def(SliderSetting::FRESHNESS).load_pos(s));
}

static void test_store_then_load_round_trips() {
    for (const Point& p : kPoints) {
        FloatStore store;
        UiSettings s(store);
        const SliderSettingDef& d = slider_setting_def(p.id);
        const int pos = d.clamp_pos(p.pos);
        d.store(s, pos);
        TEST_ASSERT_EQUAL_INT(pos, d.load_pos(s));
    }
}

static void test_clamp_pos() {
    TEST_ASSERT_EQUAL_INT(15, slider_setting_def(SliderSetting::BRIGHTNESS_NORMAL).clamp_pos(0));
    TEST_ASSERT_EQUAL_INT(60, slider_setting_def(SliderSetting::BRIGHTNESS_SCREENSAVER).clamp_pos(60));
    TEST_ASSERT_EQUAL_INT(1, slider_setting_def(SliderSetting::PURGE_AMOUNT).clamp_pos(0));
    TEST_ASSERT_EQUAL_INT(25, slider_setting_def(SliderSetting::PURGE_AMOUNT).clamp_pos(40));
    TEST_ASSERT_EQUAL_INT(0, slider_setting_def(SliderSetting::FRESHNESS).clamp_pos(-3));
    TEST_ASSERT_EQUAL_INT(8, slider_setting_def(SliderSetting::FRESHNESS).clamp_pos(12));
}

static void test_live_brightness_is_clamped_fraction() {
    const SliderSettingDef& d = slider_setting_def(SliderSetting::BRIGHTNESS_NORMAL);
    TEST_ASSERT_EQUAL_FLOAT(0.15f, d.live_brightness(5));
    TEST_ASSERT_EQUAL_FLOAT(0.6f, d.live_brightness(60));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, d.live_brightness(100));
}

void run_menu_setting_sliders_tests() {
    RUN_TEST(test_table_covers_every_slider_in_order);
    RUN_TEST(test_hook_table_is_complete);
    RUN_TEST(test_row_label_color_and_range);
    RUN_TEST(test_label_text_at_each_position);
    RUN_TEST(test_store_writes_one_float_put_of_the_legacy_value);
    RUN_TEST(test_load_pos_defaults);
    RUN_TEST(test_store_then_load_round_trips);
    RUN_TEST(test_clamp_pos);
    RUN_TEST(test_live_brightness_is_clamped_fraction);
}
