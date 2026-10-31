#include <unity.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>
#include "system/settings_store.h"
#include "system/ui_settings.h"
#include "config/user.h"

namespace {

enum class T { BOOL, INT, FLOAT };

struct Entry {
    std::string ns, key;
    T type;
    bool b = false;
    int32_t i = 0;
    float f = 0.0f;
};

// In-memory store: typed entries (a read with the wrong type returns the default, like NVS) and a put log.
class FakeStore : public SettingsStore {
public:
    std::vector<Entry> data;
    std::vector<Entry> puts;
    bool accept_puts = true;

    Entry* find(const char* ns, const char* key) {
        for (auto& e : data) if (e.ns == ns && e.key == key) return &e;
        return nullptr;
    }
    Entry& slot(const char* ns, const char* key, T type) {
        Entry* e = find(ns, key);
        if (!e) {
            data.push_back(Entry());
            e = &data.back();
            e->ns = ns;
            e->key = key;
        }
        e->type = type;
        return *e;
    }
    void seed_bool(const char* ns, const char* key, bool v) { slot(ns, key, T::BOOL).b = v; }
    void seed_int(const char* ns, const char* key, int32_t v) { slot(ns, key, T::INT).i = v; }
    void seed_float(const char* ns, const char* key, float v) { slot(ns, key, T::FLOAT).f = v; }

    bool get_bool(const char* ns, const char* key, bool d) override {
        Entry* e = find(ns, key);
        return (e && e->type == T::BOOL) ? e->b : d;
    }
    int32_t get_int(const char* ns, const char* key, int32_t d) override {
        Entry* e = find(ns, key);
        return (e && e->type == T::INT) ? e->i : d;
    }
    float get_float(const char* ns, const char* key, float d) override {
        Entry* e = find(ns, key);
        return (e && e->type == T::FLOAT) ? e->f : d;
    }
    bool put_bool(const char* ns, const char* key, bool v) override {
        Entry e; e.ns = ns; e.key = key; e.type = T::BOOL; e.b = v;
        puts.push_back(e);
        if (accept_puts) seed_bool(ns, key, v);
        return accept_puts;
    }
    bool put_int(const char* ns, const char* key, int32_t v) override {
        Entry e; e.ns = ns; e.key = key; e.type = T::INT; e.i = v;
        puts.push_back(e);
        if (accept_puts) seed_int(ns, key, v);
        return accept_puts;
    }
    bool put_float(const char* ns, const char* key, float v) override {
        Entry e; e.ns = ns; e.key = key; e.type = T::FLOAT; e.f = v;
        puts.push_back(e);
        if (accept_puts) seed_float(ns, key, v);
        return accept_puts;
    }
};

// What shipped firmware writes, do not edit.
struct GoldenRow {
    const char* ns;
    const char* key;
    T type;
    float default_value;  // bool as 0/1, int as value
};
const GoldenRow kGolden[] = {
    {"bluetooth", "startup", T::BOOL, 1.0f},
    {"logging", "enabled", T::BOOL, 0.0f},
    {"swipe", "enabled", T::BOOL, 0.0f},
    {"autogrind", "auto_start", T::BOOL, 0.0f},
    {"autogrind", "auto_return", T::BOOL, 0.0f},
    {"brightness", "normal", T::FLOAT, USER_SCREEN_BRIGHTNESS_NORMAL},
    {"brightness", "screensaver", T::FLOAT, USER_SCREEN_BRIGHTNESS_DIMMED},
    {"grinder", "grind_mode", T::INT, 0.0f},
    {"grinder", "grinder_mode", T::INT, 1.0f},
    {"grinder", "grinder_amt_g", T::FLOAT, 1.0f},
    {"grinder", "freshness_hrs", T::FLOAT, 8.0f},
};
const int kRows = sizeof(kGolden) / sizeof(kGolden[0]);

bool float_bits_equal(float a, float b) { return memcmp(&a, &b, sizeof(float)) == 0; }

// Per-row accessors, in kGolden order. Setters are null for the read-only grind_mode row.
struct Accessors {
    bool (UiSettings::*get_b)() const;
    bool (UiSettings::*set_b)(bool);
    float (UiSettings::*get_f)() const;
    bool (UiSettings::*set_f)(float);
    int (UiSettings::*get_i)() const;
    bool (UiSettings::*set_i)(int);
};
const Accessors kAcc[kRows] = {
    {&UiSettings::ble_startup, &UiSettings::set_ble_startup, nullptr, nullptr, nullptr, nullptr},
    {&UiSettings::logging_enabled, &UiSettings::set_logging_enabled, nullptr, nullptr, nullptr, nullptr},
    {&UiSettings::swipe_enabled, &UiSettings::set_swipe_enabled, nullptr, nullptr, nullptr, nullptr},
    {&UiSettings::auto_start, &UiSettings::set_auto_start, nullptr, nullptr, nullptr, nullptr},
    {&UiSettings::auto_return, &UiSettings::set_auto_return, nullptr, nullptr, nullptr, nullptr},
    {nullptr, nullptr, &UiSettings::brightness_normal, &UiSettings::set_brightness_normal, nullptr, nullptr},
    {nullptr, nullptr, &UiSettings::brightness_screensaver, &UiSettings::set_brightness_screensaver, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, &UiSettings::grind_mode_raw, nullptr},
    {nullptr, nullptr, nullptr, nullptr, &UiSettings::purge_mode, &UiSettings::set_purge_mode},
    {nullptr, nullptr, &UiSettings::purge_amount_g, &UiSettings::set_purge_amount_g, nullptr, nullptr},
    {nullptr, nullptr, &UiSettings::freshness_hours, &UiSettings::set_freshness_hours, nullptr, nullptr},
};

}  // namespace

static void test_empty_store_returns_legacy_defaults() {
    FakeStore store;
    UiSettings s(store);
    TEST_ASSERT_TRUE(s.ble_startup());
    TEST_ASSERT_FALSE(s.logging_enabled());
    TEST_ASSERT_FALSE(s.swipe_enabled());
    TEST_ASSERT_FALSE(s.auto_start());
    TEST_ASSERT_FALSE(s.auto_return());
    TEST_ASSERT_TRUE(float_bits_equal(USER_SCREEN_BRIGHTNESS_NORMAL, s.brightness_normal()));
    TEST_ASSERT_TRUE(float_bits_equal(USER_SCREEN_BRIGHTNESS_DIMMED, s.brightness_screensaver()));
    TEST_ASSERT_EQUAL_INT(0, s.grind_mode_raw());
    TEST_ASSERT_EQUAL_INT(1, s.purge_mode());
    TEST_ASSERT_TRUE(float_bits_equal(1.0f, s.purge_amount_g()));
    TEST_ASSERT_TRUE(float_bits_equal(8.0f, s.freshness_hours()));
}

static void test_setters_write_exactly_one_legacy_put() {
    for (int r = 0; r < kRows; r++) {
        const GoldenRow& g = kGolden[r];
        const Accessors& a = kAcc[r];
        if (a.set_b) {
            for (bool v : {true, false}) {
                FakeStore store;
                UiSettings s(store);
                TEST_ASSERT_TRUE((s.*a.set_b)(v));
                TEST_ASSERT_EQUAL_UINT32(1, store.puts.size());
                TEST_ASSERT_EQUAL_STRING(g.ns, store.puts[0].ns.c_str());
                TEST_ASSERT_EQUAL_STRING(g.key, store.puts[0].key.c_str());
                TEST_ASSERT_TRUE(store.puts[0].type == T::BOOL);
                TEST_ASSERT_EQUAL(v, store.puts[0].b);
            }
        }
        if (a.set_f) {
            FakeStore store;
            UiSettings s(store);
            const float v = 0.57f;
            TEST_ASSERT_TRUE((s.*a.set_f)(v));
            TEST_ASSERT_EQUAL_UINT32(1, store.puts.size());
            TEST_ASSERT_EQUAL_STRING(g.ns, store.puts[0].ns.c_str());
            TEST_ASSERT_EQUAL_STRING(g.key, store.puts[0].key.c_str());
            TEST_ASSERT_TRUE(store.puts[0].type == T::FLOAT);
            TEST_ASSERT_TRUE(float_bits_equal(v, store.puts[0].f));
        }
        if (a.set_i) {
            for (int v : {0, 1, -1}) {
                FakeStore store;
                UiSettings s(store);
                TEST_ASSERT_TRUE((s.*a.set_i)(v));
                TEST_ASSERT_EQUAL_UINT32(1, store.puts.size());
                TEST_ASSERT_EQUAL_STRING(g.ns, store.puts[0].ns.c_str());
                TEST_ASSERT_EQUAL_STRING(g.key, store.puts[0].key.c_str());
                TEST_ASSERT_TRUE(store.puts[0].type == T::INT);
                TEST_ASSERT_EQUAL_INT(v, store.puts[0].i);
            }
        }
    }
}

static void test_getters_read_legacy_ns_key_type() {
    for (int r = 0; r < kRows; r++) {
        const GoldenRow& g = kGolden[r];
        const Accessors& a = kAcc[r];
        if (a.get_b) {
            for (bool v : {true, false}) {
                FakeStore store;
                store.seed_bool(g.ns, g.key, v);
                UiSettings s(store);
                TEST_ASSERT_EQUAL(v, (s.*a.get_b)());
            }
        }
        if (a.get_f) {
            FakeStore store;
            store.seed_float(g.ns, g.key, 0.57f);
            UiSettings s(store);
            TEST_ASSERT_TRUE(float_bits_equal(0.57f, (s.*a.get_f)()));
        }
        if (a.get_i) {
            for (int v : {0, 1, -1}) {
                FakeStore store;
                store.seed_int(g.ns, g.key, v);
                UiSettings s(store);
                TEST_ASSERT_EQUAL_INT(v, (s.*a.get_i)());
            }
        }
    }
}

static void test_wrong_type_seed_returns_default() {
    for (int r = 0; r < kRows; r++) {
        const GoldenRow& g = kGolden[r];
        const Accessors& a = kAcc[r];
        FakeStore store;
        // Seed the legacy ns/key with a different type than the getter reads
        if (a.get_b) store.seed_float(g.ns, g.key, 0.5f);
        if (a.get_f) store.seed_int(g.ns, g.key, 7);
        if (a.get_i) store.seed_float(g.ns, g.key, 0.5f);
        UiSettings s(store);
        if (a.get_b) TEST_ASSERT_EQUAL(g.default_value != 0.0f, (s.*a.get_b)());
        if (a.get_f) TEST_ASSERT_TRUE(float_bits_equal(g.default_value, (s.*a.get_f)()));
        if (a.get_i) TEST_ASSERT_EQUAL_INT((int)g.default_value, (s.*a.get_i)());
    }
}

static void test_logging_and_swipe_do_not_alias() {
    FakeStore store;
    UiSettings s(store);
    s.set_logging_enabled(true);
    TEST_ASSERT_TRUE(s.logging_enabled());
    TEST_ASSERT_FALSE(s.swipe_enabled());
    s.set_swipe_enabled(true);
    s.set_logging_enabled(false);
    TEST_ASSERT_TRUE(s.swipe_enabled());
    TEST_ASSERT_FALSE(s.logging_enabled());
}

static void test_auto_start_and_return_share_ns_not_key() {
    FakeStore store;
    UiSettings s(store);
    s.set_auto_start(true);
    TEST_ASSERT_TRUE(s.auto_start());
    TEST_ASSERT_FALSE(s.auto_return());
    s.set_auto_return(true);
    s.set_auto_start(false);
    TEST_ASSERT_TRUE(s.auto_return());
    TEST_ASSERT_FALSE(s.auto_start());
    for (const auto& p : store.puts) TEST_ASSERT_EQUAL_STRING("autogrind", p.ns.c_str());
}

static void test_setters_touch_only_their_own_key() {
    FakeStore store;
    store.seed_float("brightness", "normal", 0.9f);
    store.seed_float("brightness", "screensaver", 0.4f);
    UiSettings s(store);
    s.set_brightness_normal(0.5f);
    TEST_ASSERT_TRUE(float_bits_equal(0.5f, s.brightness_normal()));
    TEST_ASSERT_TRUE(float_bits_equal(0.4f, s.brightness_screensaver()));
    s.set_purge_amount_g(2.0f);
    TEST_ASSERT_TRUE(float_bits_equal(8.0f, s.freshness_hours()));
    TEST_ASSERT_EQUAL_INT(1, s.purge_mode());
}

static void test_setter_reports_store_failure() {
    FakeStore store;
    store.accept_puts = false;
    UiSettings s(store);
    TEST_ASSERT_FALSE(s.set_ble_startup(false));
    TEST_ASSERT_FALSE(s.set_purge_mode(0));
    TEST_ASSERT_FALSE(s.set_freshness_hours(4.0f));
}

static void test_same_str() {
    using ui_settings_keys::same_str;
    static_assert(same_str("abc", "abc"), "equal strings");
    static_assert(!same_str("abc", "abd"), "different strings");
    static_assert(!same_str("abc", "ab"), "prefix is not equal");
    static_assert(same_str(ui_settings_keys::kPurgeAmountG, "grinder_amt_g"), "key pinned");
    TEST_ASSERT_TRUE(same_str("", ""));
    TEST_ASSERT_FALSE(same_str("a", ""));
}

void run_ui_settings_tests() {
    RUN_TEST(test_empty_store_returns_legacy_defaults);
    RUN_TEST(test_setters_write_exactly_one_legacy_put);
    RUN_TEST(test_getters_read_legacy_ns_key_type);
    RUN_TEST(test_wrong_type_seed_returns_default);
    RUN_TEST(test_logging_and_swipe_do_not_alias);
    RUN_TEST(test_auto_start_and_return_share_ns_not_key);
    RUN_TEST(test_setters_touch_only_their_own_key);
    RUN_TEST(test_setter_reports_store_failure);
    RUN_TEST(test_same_str);
}
