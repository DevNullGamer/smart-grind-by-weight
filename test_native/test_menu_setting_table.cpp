#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "system/menu_setting_table.h"
#include "system/settings_store.h"
#include "system/ui_settings.h"

namespace {

struct BoolPut {
    std::string ns, key;
    bool value;
};

// Bool-only in-memory store with a put log; other types are not used by the toggles.
class BoolStore : public SettingsStore {
public:
    struct Slot {
        std::string ns, key;
        bool value;
    };
    std::vector<Slot> data;
    std::vector<BoolPut> puts;
    int other_calls = 0;

    bool get_bool(const char* ns, const char* key, bool d) override {
        for (auto& s : data) if (s.ns == ns && s.key == key) return s.value;
        return d;
    }
    bool put_bool(const char* ns, const char* key, bool v) override {
        puts.push_back({ns, key, v});
        for (auto& s : data) {
            if (s.ns == ns && s.key == key) { s.value = v; return true; }
        }
        data.push_back({ns, key, v});
        return true;
    }
    int32_t get_int(const char*, const char*, int32_t d) override { other_calls++; return d; }
    bool put_int(const char*, const char*, int32_t) override { other_calls++; return false; }
    float get_float(const char*, const char*, float d) override { other_calls++; return d; }
    bool put_float(const char*, const char*, float) override { other_calls++; return false; }
};

// What shipped firmware shows and writes, do not edit.
struct Golden {
    ToggleSetting id;
    const char* label;
    const char* ns;
    const char* key;
    bool default_value;
    const char* log_on;
    const char* log_off;
    ToggleAfter after;
};
const Golden kGolden[] = {
    {ToggleSetting::BLE_STARTUP, "Startup", "bluetooth", "startup", true,
     "Bluetooth startup enabled", "Bluetooth startup disabled", ToggleAfter::NONE},
    {ToggleSetting::LOGGING, "Logging", "logging", "enabled", false,
     "Logging enabled", "Logging disabled", ToggleAfter::NONE},
    {ToggleSetting::SWIPE, "Swipe", "swipe", "enabled", false,
     "Grind mode swipe gestures enabled", "Grind mode swipe gestures disabled", ToggleAfter::NONE},
    {ToggleSetting::AUTO_START, "Start", "autogrind", "auto_start", false,
     "Auto-start on cup enabled", "Auto-start on cup disabled", ToggleAfter::REFRESH_AUTO_ACTIONS},
    {ToggleSetting::AUTO_RETURN, "Return", "autogrind", "auto_return", false,
     "Auto return on cup removal enabled", "Auto return on cup removal disabled", ToggleAfter::REFRESH_AUTO_ACTIONS},
};
const size_t kRows = sizeof(kGolden) / sizeof(kGolden[0]);

}  // namespace

static void test_table_covers_every_toggle_in_order() {
    TEST_ASSERT_EQUAL_UINT(kRows, TOGGLE_SETTING_COUNT);
    for (size_t i = 0; i < kRows; i++) {
        TEST_ASSERT_EQUAL_UINT(i, static_cast<size_t>(kGolden[i].id));
    }
}

static void test_labels_and_log_strings() {
    for (const Golden& g : kGolden) {
        const ToggleSettingDef& d = toggle_setting_def(g.id);
        TEST_ASSERT_EQUAL_STRING(g.label, d.label);
        TEST_ASSERT_EQUAL_STRING(g.log_on, d.log_on);
        TEST_ASSERT_EQUAL_STRING(g.log_off, d.log_off);
    }
}

static void test_after_only_for_auto_actions() {
    for (const Golden& g : kGolden) {
        TEST_ASSERT_TRUE(toggle_setting_def(g.id).after == g.after);
    }
}

static void test_getter_reads_legacy_default_and_ns_key() {
    for (const Golden& g : kGolden) {
        BoolStore store;
        UiSettings s(store);
        const ToggleSettingDef& d = toggle_setting_def(g.id);
        TEST_ASSERT_EQUAL(g.default_value, (s.*d.get)());

        store.data.push_back({g.ns, g.key, !g.default_value});
        TEST_ASSERT_EQUAL(!g.default_value, (s.*d.get)());
        TEST_ASSERT_EQUAL_INT(0, store.other_calls);
    }
}

static void test_setter_writes_one_bool_put_to_ns_key() {
    for (const Golden& g : kGolden) {
        for (int v = 0; v < 2; v++) {
            BoolStore store;
            UiSettings s(store);
            const ToggleSettingDef& d = toggle_setting_def(g.id);
            TEST_ASSERT_TRUE((s.*d.set)(v != 0));
            TEST_ASSERT_EQUAL_UINT(1, store.puts.size());
            TEST_ASSERT_EQUAL_STRING(g.ns, store.puts[0].ns.c_str());
            TEST_ASSERT_EQUAL_STRING(g.key, store.puts[0].key.c_str());
            TEST_ASSERT_EQUAL(v != 0, store.puts[0].value);
            TEST_ASSERT_EQUAL(v != 0, (s.*d.get)());
            TEST_ASSERT_EQUAL_INT(0, store.other_calls);
        }
    }
}

static void test_setters_do_not_alias() {
    BoolStore store;
    UiSettings s(store);
    (s.*toggle_setting_def(ToggleSetting::AUTO_START).set)(true);
    for (const Golden& g : kGolden) {
        const bool expect = (g.id == ToggleSetting::AUTO_START) ? true : g.default_value;
        TEST_ASSERT_EQUAL(expect, (s.*toggle_setting_def(g.id).get)());
    }
}

void run_menu_setting_table_tests() {
    RUN_TEST(test_table_covers_every_toggle_in_order);
    RUN_TEST(test_labels_and_log_strings);
    RUN_TEST(test_after_only_for_auto_actions);
    RUN_TEST(test_getter_reads_legacy_default_and_ns_key);
    RUN_TEST(test_setter_writes_one_bool_put_to_ns_key);
    RUN_TEST(test_setters_do_not_alias);
}
